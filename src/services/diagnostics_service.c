#include <pickup/services/diagnostics_service.h>

#include <pickup/detect/distro.h>
#include <pickup/detect/gcc_install.h>
#include <pickup/detect/health.h>
#include <pickup/detect/recipe.h>
#include <pickup/services/archive_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/inventory_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The version string a toolchain is described by in a report. */
#define VERSION_SIZE 32

const char *finding_severity_name(finding_severity severity) {
    switch (severity) {
    case finding_ok:      return "ok";
    case finding_warning: return "warning";
    case finding_error:   return "error";
    }
    return "unknown";
}

bool diagnostics_has_errors(const diagnostics_report *report) {
    for (size_t i = 0; i < report->count; i++) {
        if (report->items[i].severity == finding_error)
            return true;
    }
    return false;
}

/* --- building a report --- */

/* Start a finding and return it, or NULL when the report is full. */
static finding *open_finding(diagnostics_report *report, finding_severity severity,
                             const char *subject) {
    if (report->count == DIAGNOSTICS_MAX_FINDINGS)
        return NULL;
    finding *entry = &report->items[report->count++];
    *entry = (finding){ .severity = severity };
    (void)fs_format_path(entry->subject, sizeof entry->subject, "%s", subject);
    return entry;
}

static void set_detail(finding *entry, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void set_detail(finding *entry, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(entry->detail, sizeof entry->detail, format, args);
    va_end(args);
}

static void add_symptom(finding *entry, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void add_symptom(finding *entry, const char *format, ...) {
    if (entry->symptom_count == FINDING_MAX_SYMPTOMS)
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(entry->symptoms[entry->symptom_count++], FINDING_TEXT_MAX,
                    format, args);
    va_end(args);
}

static void add_remedy(finding *entry, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void add_remedy(finding *entry, const char *format, ...) {
    if (entry->remedy_count == FINDING_MAX_REMEDIES)
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(entry->remedies[entry->remedy_count++], FINDING_TEXT_MAX,
                    format, args);
    va_end(args);
}

/* --- the environment Pickup itself depends on --- */

static void check_tools(diagnostics_report *report) {
    if (!http_available()) {
        finding *entry = open_finding(report, finding_error, "curl");
        if (entry == NULL)
            return;
        set_detail(entry, "not found; nothing can be downloaded");
        add_remedy(entry, "install %s", http_requirement());
    }
    if (!archive_available()) {
        finding *entry = open_finding(report, finding_error, "tar");
        if (entry == NULL)
            return;
        set_detail(entry, "not found; nothing can be unpacked");
        add_remedy(entry, "install %s", archive_requirement());
    }
}

static void check_home(diagnostics_report *report) {
    char home[PICKUP_PATHS_MAX];
    if (!paths_home(home, sizeof home)) {
        finding *entry = open_finding(report, finding_error, "pickup home");
        if (entry == NULL)
            return;
        set_detail(entry, "neither $%s nor a home directory is set",
                   PICKUP_HOME_ENV);
        add_remedy(entry, "set %s to a directory pickup may write to",
                   PICKUP_HOME_ENV);
        return;
    }

    /* Only reported when it exists and cannot be used. A home that is not
       there yet is the ordinary state before the first install. */
    if (fs_path_exists(home) && !fs_is_dir(home)) {
        finding *entry = open_finding(report, finding_error, "pickup home");
        if (entry == NULL)
            return;
        (void)fs_format_path(entry->location, sizeof entry->location, "%s", home);
        set_detail(entry, "exists and is not a directory");
        add_remedy(entry, "remove it, or point %s elsewhere", PICKUP_HOME_ENV);
    }
}

/* --- what each toolchain can build --- */

/* What was learned about one toolchain, worked out once because every step of
   it costs a compile. */
typedef struct {
    link_recipe c;
    link_recipe cxx;
    gcc_install_list installs;   /* what its driver selects, when it selects */
} toolchain_survey;

static void survey_toolchain(const toolchain *chain, toolchain_survey *out) {
    *out = (toolchain_survey){ 0 };
    out->c = recipe_discover(chain, lang_c);
    out->cxx = recipe_discover(chain, lang_cxx);
    /* Only Clang borrows a GCC, and only Clang answers this. */
    if (chain->vendor == vendor_clang || chain->vendor == vendor_apple_clang)
        (void)gcc_install_query(chain->path, &out->installs);
}

/* A toolchain described the way a report names it: "clang 22.1.8". */
static void describe(const toolchain *chain, char *out, size_t out_size) {
    char version[VERSION_SIZE];
    toolchain_version_format(chain->version, version, sizeof version);
    (void)snprintf(out, out_size, "%s %s", chain->name, version);
}

/*
 * The GCC installation a Clang selected, when that installation has no C++.
 *
 * This is the cause behind two unrelated-looking symptoms, and finding it is
 * what turns "clang cannot find <iostream>" into something a reader can act
 * on. Returns the path, or NULL when the driver's choice was a sound one.
 */
static const char *incomplete_selection(const toolchain_survey *survey) {
    const gcc_install_list *installs = &survey->installs;
    if (installs->selected == GCC_INSTALL_NONE
        || installs->selected >= installs->count)
        return NULL;

    const gcc_install *selected = &installs->items[installs->selected];
    return selected->has_libstdcxx ? NULL : selected->path;
}

/* The last path component, which for a GCC installation is its version. */
static const char *version_of_path(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* How far above a GCC installation directory its prefix sits:
   <prefix>/lib/gcc/<triple>/<version> is four levels down. */
#define GCC_INSTALL_PREFIX_DEPTH 4

/* Climb `levels` path components, to reach the prefix an installation belongs
   to. False when the path is too shallow to have one. */
static bool climb(const char *path, int levels, char *out, size_t out_size) {
    if (!fs_format_path(out, out_size, "%s", path))
        return false;
    for (int i = 0; i < levels; i++) {
        char *slash = strrchr(out, '/');
        if (slash == NULL || slash == out)
            return false;
        *slash = '\0';
    }
    return out[0] != '\0';
}

/* Name the package that completes a GCC, when the distribution has one. */
static void suggest_gxx_package(finding *entry, distro_family family, int major) {
    char package[DISTRO_PACKAGE_MAX];
    if (!distro_gxx_package(family, major, package, sizeof package))
        return;
    /* Phrased as a suggestion. Pickup is not the system's package manager and
       must not read as though it were issuing commands to one. */
    add_remedy(entry, "install the %s package - completes this GCC, "
                      "and fixes everything above", package);
}

/*
 * One finding per incomplete GCC installation, carrying every compiler it
 * spoils.
 *
 * Built by walking the toolchains twice: once to find the installations that
 * are broken, and once to collect what each one broke. That is what keeps a
 * cause from being reported as several unrelated faults.
 */
static void check_gcc_installations(const inventory *list,
                                    const toolchain_survey *surveys,
                                    distro_family family,
                                    diagnostics_report *report,
                                    bool *explained) {
    for (size_t i = 0; i < list->count; i++) {
        const char *broken = incomplete_selection(&surveys[i]);
        if (broken == NULL || surveys[i].cxx.usable)
            continue;

        /* Already reported through an earlier toolchain that selects the very
           same installation. */
        bool seen = false;
        for (size_t j = 0; j < report->count && !seen; j++)
            seen = strcmp(report->items[j].location, broken) == 0;
        if (seen) {
            explained[i] = true;
            continue;
        }

        const char *version = version_of_path(broken);
        char subject[FINDING_SUBJECT_MAX];
        (void)snprintf(subject, sizeof subject, "gcc %s", version);

        finding *entry = open_finding(report, finding_error, subject);
        if (entry == NULL)
            return;
        (void)fs_format_path(entry->location, sizeof entry->location, "%s", broken);
        set_detail(entry, "installed without C++ support: no libstdc++ headers");

        /* Every toolchain this installation spoils, named. */
        for (size_t j = 0; j < list->count; j++) {
            const char *other = incomplete_selection(&surveys[j]);
            if (other == NULL || strcmp(other, broken) != 0 || surveys[j].cxx.usable)
                continue;
            char name[FINDING_SUBJECT_MAX];
            describe(&list->items[j], name, sizeof name);
            add_symptom(entry, "%s selects this GCC and cannot find <iostream>", name);
            explained[j] = true;
        }

        /* And the GCC of that version, if it is on the machine without a C++
           driver: the same missing package, showing up somewhere else.

           Belonging to the installation is what qualifies a compiler here, not
           sharing its version number. A GCC 12 that Pickup installed under its
           own home has nothing to do with a broken /usr/lib/gcc/…/12, and
           listing it as a symptom would blame one thing for another's fault. */
        char prefix[PICKUP_PATHS_MAX];
        bool have_prefix = climb(broken, GCC_INSTALL_PREFIX_DEPTH,
                                 prefix, sizeof prefix);

        for (size_t j = 0; j < list->count; j++) {
            const toolchain *chain = &list->items[j];
            char major[VERSION_SIZE];
            (void)snprintf(major, sizeof major, "%d", chain->version.major);
            if (chain->vendor != vendor_gcc || chain->cxx_path[0] != '\0'
                || strcmp(major, version) != 0)
                continue;
            if (!have_prefix || strncmp(chain->path, prefix, strlen(prefix)) != 0)
                continue;
            char name[FINDING_SUBJECT_MAX];
            describe(chain, name, sizeof name);
            add_symptom(entry, "%s has no C++ driver, so it compiles C only", name);
            explained[j] = true;
        }

        int major = 0;
        (void)sscanf(version, "%d", &major);
        suggest_gxx_package(entry, family, major);
        /* The other way out, and it does something different: it leaves the
           broken installation exactly as it is. Saying so is the difference
           between two remedies and two ways of describing one. */
        add_remedy(entry, "pickup install gcc --version %d - a separate "
                          "toolchain, no root; leaves the one above untouched",
                   major);
    }
}

/* Whatever is left: a toolchain that cannot build and was not explained by an
   incomplete GCC. */
static void check_remaining(const inventory *list, const toolchain_survey *surveys,
                            const bool *explained, diagnostics_report *report) {
    for (size_t i = 0; i < list->count; i++) {
        const toolchain *chain = &list->items[i];
        if (explained[i])
            continue;

        char subject[FINDING_SUBJECT_MAX];
        describe(chain, subject, sizeof subject);

        if (!surveys[i].c.usable) {
            /* A compiler that cannot build C is not usable for anything. */
            finding *entry = open_finding(report, finding_error, subject);
            if (entry == NULL)
                return;
            (void)fs_format_path(entry->location, sizeof entry->location,
                                 "%s", chain->path);
            set_detail(entry, "cannot compile, link and run a C program");
            continue;
        }

        /* No C++ driver at all is ordinary for a C compiler, and not a fault. */
        if (chain->cxx_path[0] == '\0' || surveys[i].cxx.usable)
            continue;

        finding *entry = open_finding(report, finding_warning, subject);
        if (entry == NULL)
            return;
        (void)fs_format_path(entry->location, sizeof entry->location,
                             "%s", chain->cxx_path);
        set_detail(entry, "builds C, and cannot build C++ in any configuration "
                          "pickup knows to offer");
    }
}

bool diagnostics_examine(const inventory *list, distro_family family,
                         diagnostics_report *out) {
    if (list->count == 0) {
        finding *entry = open_finding(out, finding_error, "compilers");
        if (entry != NULL) {
            set_detail(entry, "none found on this machine");
            add_remedy(entry, "pickup install clang");
        }
        return true;
    }

    toolchain_survey *surveys = calloc(list->count, sizeof *surveys);
    bool *explained = calloc(list->count, sizeof *explained);
    if (surveys == NULL || explained == NULL) {
        free(surveys);
        free(explained);
        return false;
    }

    for (size_t i = 0; i < list->count; i++)
        survey_toolchain(&list->items[i], &surveys[i]);

    /* Causes before leftovers, so that a toolchain already accounted for by a
       broken GCC is not reported a second time on its own. */
    check_gcc_installations(list, surveys, family, out, explained);
    check_remaining(list, surveys, explained, out);

    free(surveys);
    free(explained);
    return true;
}

bool diagnostics_run(diagnostics_report *out) {
    *out = (diagnostics_report){ .count = 0 };

    check_tools(out);
    check_home(out);

    inventory list;
    if (!inventory_load(&list, false))
        return false;

    bool examined = diagnostics_examine(&list, distro_detect(), out);
    inventory_free(&list);
    return examined;
}
