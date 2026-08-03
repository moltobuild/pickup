#include <pickup/commands/doctor_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/diagnostics_service.h>
#include <pickup/util/color.h>

#include <stdio.h>

/* Marks in front of a finding. The severity is also spelled out in the TOML
   form, so nothing depends on reading a symbol. */
#define MARK_ERROR   "x"
#define MARK_WARNING "!"
#define MARK_OK      "-"

/* What is said when there is nothing to say. */
#define NOTHING_WRONG "everything pickup checks is in order"

static const char *mark_of(finding_severity severity) {
    switch (severity) {
    case finding_error:   return MARK_ERROR;
    case finding_warning: return MARK_WARNING;
    case finding_ok:      return MARK_OK;
    }
    return MARK_OK;
}

static const char *color_of(finding_severity severity) {
    switch (severity) {
    case finding_error:   return color_error();
    case finding_warning: return color_accent();
    case finding_ok:      return color_ok();
    }
    return "";
}

static void print_finding_text(const finding *entry) {
    printf("%s%s%s  %s", color_of(entry->severity), mark_of(entry->severity),
           color_reset(), entry->subject);
    if (entry->location[0] != '\0')
        printf("  %s%s%s", color_dim(), entry->location, color_reset());
    printf("\n");

    if (entry->detail[0] != '\0')
        printf("      %s\n", entry->detail);

    /* Indented under the cause, because that is what they are: the same fault
       seen from somewhere else. */
    for (size_t i = 0; i < entry->symptom_count; i++)
        printf("        %s\n", entry->symptoms[i]);

    for (size_t i = 0; i < entry->remedy_count; i++)
        printf("      %s->%s %s\n", color_accent(), color_reset(),
               entry->remedies[i]);
}

static void print_text(const diagnostics_report *report) {
    if (report->count == 0) {
        printf("%s\n", NOTHING_WRONG);
        return;
    }
    for (size_t i = 0; i < report->count; i++) {
        if (i > 0)
            printf("\n");
        print_finding_text(&report->items[i]);
    }
}

/* Print an array of strings as a TOML array, one entry per line so that a long
   remedy does not produce an unreadable line. */
static void print_toml_array(const char *key, const char rows[][FINDING_TEXT_MAX],
                             size_t count) {
    printf("%s = [", key);
    for (size_t i = 0; i < count; i++)
        printf("%s\n  \"%s\"", i == 0 ? "" : ",", rows[i]);
    printf("%s]\n", count == 0 ? "" : "\n");
}

static void print_toml(const diagnostics_report *report) {
    for (size_t i = 0; i < report->count; i++) {
        const finding *entry = &report->items[i];
        printf("[[finding]]\n");
        printf("severity = \"%s\"\n", finding_severity_name(entry->severity));
        printf("subject = \"%s\"\n", entry->subject);
        printf("location = \"%s\"\n", entry->location);
        printf("detail = \"%s\"\n", entry->detail);
        print_toml_array("symptoms", entry->symptoms, entry->symptom_count);
        print_toml_array("remedies", entry->remedies, entry->remedy_count);
        if (i + 1 < report->count)
            printf("\n");
    }
}

int doctor_command_run(bool as_toml) {
    diagnostics_report report;
    if (!diagnostics_run(&report)) {
        fprintf(stderr, "pickup: could not examine this machine\n");
        return exit_failure;
    }

    if (as_toml)
        print_toml(&report);
    else
        print_text(&report);

    /* A warning leaves the exit code at zero: the machine can still build, and
       a caller that stops on any remark would stop on things that do not
       matter to it. */
    return diagnostics_has_errors(&report) ? exit_failure : exit_ok;
}
