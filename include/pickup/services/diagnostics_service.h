#ifndef PICKUP_DIAGNOSTICS_SERVICE_H
#define PICKUP_DIAGNOSTICS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/detect/distro.h>
#include <pickup/services/inventory_service.h>
#include <pickup/services/paths_service.h>

/*
 * What is wrong with this machine, from the point of view of building code.
 *
 * The organising rule is that a finding is a cause, not a symptom. One GCC
 * installed without its C++ half shows up in two places — as a `g++` that is
 * not there, and as a Clang that cannot find <iostream> — and reporting those
 * separately would send a reader chasing two problems where there is one. It
 * would also hide the only part that was not obvious: why a package missing
 * from GCC breaks Clang.
 *
 * So the subject of a finding is the thing that is broken, the compilers it
 * spoils hang off it as symptoms, and the remedies say what to do. Remedies
 * are named, never applied: repairing the system is not what a diagnosis is
 * for, and the two most useful remedies here are usually not equivalent — one
 * fixes what is broken, the other sidesteps it — so which to take is a
 * decision that belongs to the reader.
 */

typedef enum {
    finding_ok,      /* worth stating, nothing to do */
    finding_warning, /* works, but not the way it should */
    finding_error,   /* something cannot be built */
} finding_severity;

/*
 * Where a finding belongs in the report.
 *
 * The sections are the questions someone actually has about a machine: what
 * can it compile, what can it be worked on with, and is Pickup itself able to
 * operate here.
 */
typedef enum {
    section_compilers,
    section_tools,
    section_environment,
} finding_section;

#define FINDING_TEXT_MAX 512
#define FINDING_SUBJECT_MAX 128
#define FINDING_MAX_SYMPTOMS 16
#define FINDING_MAX_REMEDIES 4
#define DIAGNOSTICS_MAX_FINDINGS 64

typedef struct {
    finding_severity severity;
    finding_section section;

    /*
     * Whether this is what stops someone from working.
     *
     * A thing can be broken and not matter. A GCC installed without its C++
     * half is broken and will stay broken, but on a machine with three other
     * toolchains that build C and C++ it prevents nothing; reporting it as a
     * failure spends the reader's attention on something they need not act on
     * and buries whatever actually does need doing.
     *
     * So this, and not the severity, is what the exit code and the default
     * output are built on. `--all` shows everything regardless: what is left
     * out is left out because it is not urgent, never because it is untrue.
     */
    bool blocking;

    char subject[FINDING_SUBJECT_MAX]; /* "gcc 12", "curl", "pickup home" */
    char location[PICKUP_PATHS_MAX];   /* where it is; "" when it has no place */
    char detail[FINDING_TEXT_MAX];     /* what is wrong with it */

    /* What it spoils. Empty when the subject is the only thing affected. */
    char symptoms[FINDING_MAX_SYMPTOMS][FINDING_TEXT_MAX];
    size_t symptom_count;

    /* What would fix it, most direct first. Each carries its own consequence,
       because they rarely do the same thing. */
    char remedies[FINDING_MAX_REMEDIES][FINDING_TEXT_MAX];
    size_t remedy_count;
} finding;

/* What a section says when nothing in it is wrong: the summary lines that make
   a report useful even on a healthy machine. Room for every section to speak
   and for a toolchain or two to report having been brought up to date. */
#define SUMMARY_MAX 12

typedef struct {
    finding items[DIAGNOSTICS_MAX_FINDINGS];
    size_t count;

    /*
     * The state of things, said plainly, whether or not anything is wrong.
     *
     * A report that only lists problems answers "what is broken" and leaves
     * "what does this machine have" unanswered — which is the question asked
     * far more often. How many compilers there are and which versions they
     * span is not a finding; it is the answer.
     */
    char summary[SUMMARY_MAX][FINDING_TEXT_MAX];
    finding_section summary_section[SUMMARY_MAX];
    size_t summary_count;
} diagnostics_report;

/* The name of a section, for reports. Never NULL. */
[[nodiscard]] const char *finding_section_name(finding_section section);

/* True if anything in the report stops this machine from being worked on. This
   is what the exit code is built on, not the severities. */
[[nodiscard]] bool diagnostics_is_blocked(const diagnostics_report *report);

/* Told which toolchain is being examined, and how far along it is.

   Examining means asking every compiler to build, link and run something,
   several times over while the flags that make it work are found. That is
   seconds of silence otherwise, and silence is what makes people think a
   command has hung. */
typedef void (*diagnostics_watch)(const char *subject, size_t done, size_t total, void *context);

/* Examine the machine and fill `out`. False only when the inventory could not
   be built at all; a machine with nothing wrong yields an empty report, which
   is a result rather than a failure. */
[[nodiscard]] bool diagnostics_run(diagnostics_report *out);

/* The same, reporting as it goes. A NULL `watch` is the quiet one. */
[[nodiscard]] bool diagnostics_run_watched(diagnostics_watch watch, void *context,
                                           diagnostics_report *out);

/* Examine an inventory that has already been built, against a known
   distribution.

   Split out because what this decides — which faults are the same fault — is
   the part worth testing, and it cannot be tested against whatever compilers
   the machine running the tests happens to have. Both parameters are inputs
   rather than things to go and find, so a caller can present a machine that
   does not exist. */
[[nodiscard]] bool diagnostics_examine(const inventory *list, distro_family family,
                                       diagnostics_report *out);

/* True if anything in the report means something cannot be built. */
[[nodiscard]] bool diagnostics_has_errors(const diagnostics_report *report);

/* The name of a severity, for reports. Never NULL. */
[[nodiscard]] const char *finding_severity_name(finding_severity severity);

#endif /* PICKUP_DIAGNOSTICS_SERVICE_H */
