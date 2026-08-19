#include <pickup/commands/doctor_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/diagnostics_service.h>
#include <pickup/util/color.h>
#include <pickup/util/format.h>
#include <pickup/util/progress.h>

#include <stdio.h>
#include <string.h>

/* What is said when there is nothing to say. */
#define NOTHING_WRONG "everything pickup checks is in order"

/* How many lines of a finding are indented under its heading. */
#define INDENT "      "
#define DEEP_INDENT "        "

static const char *color_of(finding_severity severity) {
    switch (severity) {
    case finding_error:
        return color_error();
    case finding_warning:
        return color_accent();
    case finding_ok:
        return color_ok();
    }
    return "";
}

/* The mark in front of a line. What decides it is whether the reader is
   stopped, not how bad the thing is in the abstract. */
static const char *mark_of(const finding *entry) {
    return entry->blocking ? format_cross() : format_check();
}

/* Width the subject is padded to when the detail follows it on the same line,
   so a run of short findings reads as a column. */
#define SUBJECT_COLUMN 10

static void print_finding(const finding *entry) {
    /* A finding with nothing but a short detail fits on one line, and reads
       better for it. One with a location, or with symptoms hanging off it, has
       more to say than a line holds. */
    bool one_line = entry->location[0] == '\0' && entry->symptom_count == 0;

    printf("  %s%s%s %s", color_of(entry->severity), mark_of(entry), color_reset(), entry->subject);

    if (one_line) {
        printf("%*s%s\n",
               (int)(entry->subject[0] != '\0' && strlen(entry->subject) < SUBJECT_COLUMN
                         ? SUBJECT_COLUMN - strlen(entry->subject)
                         : 1),
               "", entry->detail);
        for (size_t i = 0; i < entry->remedy_count; i++)
            printf(INDENT "%s->%s %s\n", color_accent(), color_reset(), entry->remedies[i]);
        return;
    }

    if (entry->location[0] != '\0')
        printf("  %s%s%s", color_dim(), entry->location, color_reset());
    printf("\n");

    if (entry->detail[0] != '\0')
        printf(INDENT "%s\n", entry->detail);

    /* Indented under the cause, because that is what they are: the same fault
       seen from somewhere else. */
    for (size_t i = 0; i < entry->symptom_count; i++)
        printf(DEEP_INDENT "%s\n", entry->symptoms[i]);

    for (size_t i = 0; i < entry->remedy_count; i++)
        printf(INDENT "%s->%s %s\n", color_accent(), color_reset(), entry->remedies[i]);
}

/* True if this finding belongs in the output as asked for. */
static bool worth_showing(const finding *entry, bool all) { return all || entry->blocking; }

/* Everything a section has to say, or nothing at all when it has none. */
static bool section_has_content(const diagnostics_report *report, finding_section section,
                                bool all) {
    for (size_t i = 0; i < report->summary_count; i++) {
        if (report->summary_section[i] == section)
            return true;
    }
    for (size_t i = 0; i < report->count; i++) {
        if (report->items[i].section == section && worth_showing(&report->items[i], all))
            return true;
    }
    return false;
}

static void print_section(const diagnostics_report *report, finding_section section, bool all) {
    if (!section_has_content(report, section, all))
        return;

    printf("%s\n", finding_section_name(section));

    /* How things stand first, then what is wrong with them: a reader wants the
       state of the machine before the exceptions to it. */
    for (size_t i = 0; i < report->summary_count; i++) {
        if (report->summary_section[i] != section)
            continue;
        printf("  %s%s%s %s\n", color_ok(), format_check(), color_reset(), report->summary[i]);
    }
    for (size_t i = 0; i < report->count; i++) {
        if (report->items[i].section == section && worth_showing(&report->items[i], all))
            print_finding(&report->items[i]);
    }
    printf("\n");
}

static void print_text(const diagnostics_report *report, bool all) {
    static const finding_section order[] = {
        section_compilers,
        section_tools,
        section_environment,
    };

    bool anything = false;
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        if (section_has_content(report, order[i], all)) {
            print_section(report, order[i], all);
            anything = true;
        }
    }
    if (!anything)
        printf("%s\n", NOTHING_WRONG);

    /* Said once, at the end, rather than left for the reader to wonder about:
       a report that shows nothing wrong on a machine with something wrong is
       hiding it, and hiding it silently is worse than not hiding it. */
    if (!all) {
        size_t hidden = 0;
        for (size_t i = 0; i < report->count; i++) {
            if (!report->items[i].blocking)
                hidden++;
        }
        if (hidden > 0)
            printf("%s%zu more thing%s worth knowing; pickup doctor --all%s\n", color_dim(), hidden,
                   hidden == 1 ? "" : "s", color_reset());
    }
}

/* Print an array of strings as a TOML array, one entry per line so that a long
   remedy does not produce an unreadable line. */
static void print_toml_array(const char *key, const char rows[][FINDING_TEXT_MAX], size_t count) {
    printf("%s = [", key);
    for (size_t i = 0; i < count; i++)
        printf("%s\n  \"%s\"", i == 0 ? "" : ",", rows[i]);
    printf("%s]\n", count == 0 ? "" : "\n");
}

/* The machine form carries everything, hidden or not, and says of each one
   whether it blocks. A consumer decides for itself what is grave; hiding rows
   from it would make the two formats disagree about the same machine. */
static void print_toml(const diagnostics_report *report) {
    for (size_t i = 0; i < report->summary_count; i++) {
        printf("[[summary]]\n");
        printf("section = \"%s\"\n", finding_section_name(report->summary_section[i]));
        printf("detail = \"%s\"\n\n", report->summary[i]);
    }

    for (size_t i = 0; i < report->count; i++) {
        const finding *entry = &report->items[i];
        printf("[[finding]]\n");
        printf("section = \"%s\"\n", finding_section_name(entry->section));
        printf("severity = \"%s\"\n", finding_severity_name(entry->severity));
        printf("blocking = %s\n", entry->blocking ? "true" : "false");
        printf("subject = \"%s\"\n", entry->subject);
        printf("location = \"%s\"\n", entry->location);
        printf("detail = \"%s\"\n", entry->detail);
        print_toml_array("symptoms", entry->symptoms, entry->symptom_count);
        print_toml_array("remedies", entry->remedies, entry->remedy_count);
        if (i + 1 < report->count)
            printf("\n");
    }
}

/* What the bar says while every compiler is made to build something. */
#define EXAMINE_LABEL "examining toolchains"

/* On stderr, for the same reason the report is on stdout: one of them is the
   answer and the other is not. */
static void watch_examine(const char *subject, size_t done, size_t total, void *context) {
    progress_line *line = context;
    (void)subject;
    if (!progress_is_interactive(stderr))
        return;
    progress_draw_steps(stderr, EXAMINE_LABEL, (long long)done, (long long)total);
    line->drawn = true;
}

int doctor_command_run(bool as_toml, bool all) {
    diagnostics_report report;
    progress_line line = {.drawn = false};
    bool examined = diagnostics_run_watched(watch_examine, &line, &report);
    /* Wiped whether it worked or not: a failure prints a message of its own,
       and half a bar in front of it reads as part of the message. */
    progress_line_clear(stderr, &line);

    if (!examined) {
        fprintf(stderr, "pickup: could not examine this machine\n");
        return exit_failure;
    }

    if (as_toml)
        print_toml(&report);
    else
        print_text(&report, all);

    /*
     * Failure means the machine cannot be worked on, not that something on it
     * is imperfect.
     *
     * A GCC installed without its C++ half is broken whatever else is present,
     * but on a machine with other toolchains that build C and C++ it stops
     * nobody — and a command that exits non-zero over it would be a command no
     * script could act on.
     */
    return diagnostics_is_blocked(&report) ? exit_failure : exit_ok;
}
