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
    finding_ok,       /* worth stating, nothing to do */
    finding_warning,  /* works, but not the way it should */
    finding_error,    /* something cannot be built */
} finding_severity;

#define FINDING_TEXT_MAX     512
#define FINDING_SUBJECT_MAX  128
#define FINDING_MAX_SYMPTOMS 16
#define FINDING_MAX_REMEDIES 4
#define DIAGNOSTICS_MAX_FINDINGS 64

typedef struct {
    finding_severity severity;
    char subject[FINDING_SUBJECT_MAX];   /* "gcc 12", "curl", "pickup home" */
    char location[PICKUP_PATHS_MAX];     /* where it is; "" when it has no place */
    char detail[FINDING_TEXT_MAX];       /* what is wrong with it */

    /* What it spoils. Empty when the subject is the only thing affected. */
    char symptoms[FINDING_MAX_SYMPTOMS][FINDING_TEXT_MAX];
    size_t symptom_count;

    /* What would fix it, most direct first. Each carries its own consequence,
       because they rarely do the same thing. */
    char remedies[FINDING_MAX_REMEDIES][FINDING_TEXT_MAX];
    size_t remedy_count;
} finding;

typedef struct {
    finding items[DIAGNOSTICS_MAX_FINDINGS];
    size_t count;
} diagnostics_report;

/* Examine the machine and fill `out`. False only when the inventory could not
   be built at all; a machine with nothing wrong yields an empty report, which
   is a result rather than a failure. */
[[nodiscard]] bool diagnostics_run(diagnostics_report *out);

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
