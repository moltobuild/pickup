#include <pickup/services/diagnostics_service.h>

#include <pickup/detect/distro.h>
#include <pickup/detect/gcc_install.h>
#include <pickup/detect/health.h>
#include <pickup/detect/tools.h>
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

static const char *const section_names[] = {
    [section_compilers]   = "compilers",
    [section_tools]       = "tools",
    [section_environment] = "environment",
};

#define SECTION_COUNT (sizeof section_names / sizeof section_names[0])

const char *finding_section_name(finding_section section) {
    if ((size_t)section >= SECTION_COUNT)
        return section_names[section_environment];
    return section_names[section];
}

bool diagnostics_is_blocked(const diagnostics_report *report) {
    for (size_t i = 0; i < report->count; i++) {
        if (report->items[i].blocking)
            return true;
    }
    return false;
}

/* Add a line stating how something stands, whether or not it is a problem. */
static void add_summary(diagnostics_report *report, finding_section section,
                        const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void add_summary(diagnostics_report *report, finding_section section,
                        const char *format, ...) {
    if (report->summary_count == SUMMARY_MAX)
        return;
    report->summary_section[report->summary_count] = section;

    va_list args;
    va_start(args, format);
    (void)vsnprintf(report->summary[report->summary_count], FINDING_TEXT_MAX,
                    format, args);
    va_end(args);
    report->summary_count++;
}

/* --- building a report --- */

/* Start a finding and return it, or NULL when the report is full. */
static finding *open_finding(diagnostics_report *report, finding_severity severity,
                             finding_section section, bool blocking,
                             const char *subject) {
    if (report->count == DIAGNOSTICS_MAX_FINDINGS)
        return NULL;
    finding *entry = &report->items[report->count++];
    *entry = (finding){
        .severity = severity, .section = section, .blocking = blocking,
    };
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

static void check_environment(diagnostics_report *report) {
    if (!http_available()) {
        finding *entry = open_finding(report, finding_error, section_environment, true, "curl");
        if (entry == NULL)
            return;
        set_detail(entry, "not found; nothing can be downloaded");
        add_remedy(entry, "install %s", http_requirement());
    }
    if (!archive_available()) {
        finding *entry = open_finding(report, finding_error, section_environment, true, "tar");
        if (entry == NULL)
            return;
        set_detail(entry, "not found; nothing can be unpacked");
        add_remedy(entry, "install %s", archive_requirement());
        return;
    }
    /* Everything the registry publishes is packed with zstd, so a tar that
       cannot open one leaves nothing installable even though tar is there.
       Only asked once tar is known to exist, or the answer would be about the
       wrong missing program. */
    if (!archive_supports_zstd()) {
        finding *entry = open_finding(report, finding_error, section_environment, true, "zstd");
        if (entry == NULL)
            return;
        set_detail(entry, "tar cannot open a zstd archive; nothing can be installed");
        add_remedy(entry, "install %s", archive_zstd_requirement());
    }
}

static void check_home(diagnostics_report *report) {
    char home[PICKUP_PATHS_MAX];
    if (!paths_home(home, sizeof home)) {
        finding *entry = open_finding(report, finding_error, section_environment, true, "pickup home");
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
        finding *entry = open_finding(report, finding_error, section_environment, true, "pickup home");
        if (entry == NULL)
            return;
        (void)fs_format_path(entry->location, sizeof entry->location, "%s", home);
        set_detail(entry, "exists and is not a directory");
        add_remedy(entry, "remove it, or point %s elsewhere", PICKUP_HOME_ENV);
    }
}

/* --- what a project is worked on with --- */

/*
 * Formatter and linter.
 *
 * Neither blocks a build, and both are missing from a machine far more often
 * than a compiler is: a report that never mentions them answers "can this
 * machine compile" and leaves "can this machine be worked on" unasked, which
 * is the question with the shorter answer and the more useful one.
 */
static void check_dev_tools(diagnostics_report *report) {
    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);

    const tool_kind kinds[] = { tool_formatter, tool_linter };
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        const dev_tool *have = NULL;
        for (size_t i = 0; i < count && have == NULL; i++) {
            if (found[i].kind == kinds[k])
                have = &found[i];
        }

        if (have != NULL) {
            add_summary(report, section_tools, "%-10s %s",
                        tool_kind_name(kinds[k]), have->version);
            continue;
        }

        /* Missing, and nothing covers it — which is the same rule the
           compilers are judged by. A broken GCC beside three working
           toolchains stops nobody; a formatter that is not there has no
           stand-in, so it is a thing this machine cannot do. */
        finding *entry = open_finding(report, finding_error, section_tools,
                                      true, tool_kind_name(kinds[k]));
        if (entry == NULL)
            return;
        set_detail(entry, "none found");
        add_remedy(entry, "pickup install %s", tool_kind_package(kinds[k]));
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
        /* Asked as the driver is actually invoked, config file and all: what
           it does is the thing being diagnosed. */
        (void)gcc_install_query(chain->path, false, &out->installs);
}

/*
 * Put a toolchain's own configuration back in step with what now works.
 *
 * A `.cfg` records a decision made at install time and nothing revalidates it,
 * so it can go on naming a GCC that has since lost its C++ half, or miss a
 * newer one that has since gained it. The recipes above were just worked out
 * afresh, so bringing the file into line costs nothing more than writing it.
 *
 * This is the one thing a diagnosis does rather than merely reports, and the
 * line it does not cross is the same one `install` respects: Pickup maintains
 * what Pickup installed and never touches the system. Repairing a GCC in /usr
 * is the reader's decision; keeping a file under the pickup home in step with
 * reality is not a decision at all.
 *
 * Whatever changes is said out loud. Nothing here happens quietly.
 */
static void refresh_configuration(const toolchain *chain,
                                  const toolchain_survey *survey,
                                  diagnostics_report *report) {
    /* Nothing outside the pickup home, and nothing that does not read a
       configuration file in the first place. */
    if (chain->source != toolchain_source_pickup)
        return;
    if (chain->vendor != vendor_clang && chain->vendor != vendor_apple_clang)
        return;

    bool changed = false;
    if (chain->cxx_path[0] != '\0')
        changed = recipe_refresh_config(chain->cxx_path, &survey->cxx);
    changed = recipe_refresh_config(chain->path, &survey->c) || changed;
    if (!changed)
        return;

    /* Which standard library it now stands on comes first, because that is the
       change a reader has to understand. A GCC named beside it is only where
       the startup objects come from, and saying that instead would report the
       part that did not move. */
    if (survey->cxx.stdlib == stdlib_libcxx && survey->cxx.runtime_count > 0) {
        add_summary(report, section_compilers,
                    "%s reconfigured - now uses its own %s, from %s",
                    chain->id, recipe_stdlib_name(survey->cxx.stdlib),
                    survey->cxx.runtime_dirs[0]);
        return;
    }

    const char *pinned = recipe_gcc_flag(&survey->cxx);
    if (pinned == NULL)
        pinned = recipe_gcc_flag(&survey->c);

    /* The directory, not the flag carrying it: a reader wants to know which
       GCC it now stands on, and the spelling of the option is noise. */
    const char *value = pinned != NULL ? strchr(pinned, '=') : NULL;
    if (value != NULL)
        add_summary(report, section_compilers, "%s reconfigured - now uses %s",
                    chain->id, value + 1);
    else
        add_summary(report, section_compilers, "%s reconfigured", chain->id);
}

/* A toolchain described the way a report names it: "clang@22.1.8". */
static void describe(const toolchain *chain, char *out, size_t out_size) {
    (void)snprintf(out, out_size, "%s", chain->id);
}

/*
 * What this machine can build, taken as a whole.
 *
 * A fault matters when nothing else covers it. One broken compiler among four
 * working ones stops nobody; the same compiler on a machine where it is the
 * only one stops everything. Counting first is what lets every check below
 * answer that question instead of guessing at it.
 */
typedef struct {
    size_t total;
    size_t builds_c;
    size_t builds_cxx;
} coverage;

/*
 * The compilers, said as a range per vendor.
 *
 * "gcc 9.5.0 to 12.3.0" is how people hold this in their heads, and it fits on
 * one line where six rows would not. The inventory is already deduplicated and
 * ordered newest first, so the ends of each run are the ends of the range and
 * nothing has to be probed to say it.
 */
static void summarise_vendors(const inventory *list, char *out, size_t out_size) {
    out[0] = '\0';
    size_t used = 0;

    for (size_t i = 0; i < list->count; ) {
        toolchain_vendor vendor = list->items[i].vendor;
        size_t last = i;
        while (last + 1 < list->count && list->items[last + 1].vendor == vendor)
            last++;

        char newest[VERSION_SIZE], oldest[VERSION_SIZE];
        toolchain_version_format(list->items[i].version, newest, sizeof newest);
        toolchain_version_format(list->items[last].version, oldest, sizeof oldest);

        int written = snprintf(out + used, out_size - used, "%s%s %s",
                               used == 0 ? "" : ", ",
                               toolchain_vendor_name(vendor), oldest);
        if (written > 0 && (size_t)written < out_size - used)
            used += (size_t)written;
        /* One version is not a range, and saying "9.5.0 to 9.5.0" reads like a
           mistake. */
        if (last != i) {
            written = snprintf(out + used, out_size - used, " to %s", newest);
            if (written > 0 && (size_t)written < out_size - used)
                used += (size_t)written;
        }
        i = last + 1;
    }
}

static void summarise_compilers(const inventory *list, coverage found,
                                diagnostics_report *report) {
    char vendors[FINDING_TEXT_MAX];
    summarise_vendors(list, vendors, sizeof vendors);
    add_summary(report, section_compilers, "%zu found - %s", found.total, vendors);

    /* The line that answers "is there one that builds C and not C++?" without
       listing them: when this is short of the total, there are, and --all
       names them. */
    add_summary(report, section_compilers, "%zu of them build C and C++",
                found.builds_cxx);
}

static coverage measure(const inventory *list, const toolchain_survey *surveys) {
    coverage found = { .total = list->count };
    for (size_t i = 0; i < list->count; i++) {
        if (surveys[i].c.usable)
            found.builds_c++;
        if (surveys[i].cxx.usable)
            found.builds_cxx++;
    }
    return found;
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
                                    distro_family family, coverage found,
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

        /* Broken, and blocking only where nothing else builds C++. The
           installation is just as incomplete either way; what changes is
           whether the reader has anything to do about it today. */
        bool blocks = found.builds_cxx == 0;
        finding *entry = open_finding(report, blocks ? finding_error : finding_warning,
                                      section_compilers, blocks, subject);
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
                            const bool *explained, coverage found,
                            diagnostics_report *report) {
    for (size_t i = 0; i < list->count; i++) {
        const toolchain *chain = &list->items[i];
        if (explained[i])
            continue;

        char subject[FINDING_SUBJECT_MAX];
        describe(chain, subject, sizeof subject);

        if (!surveys[i].c.usable) {
            /* A compiler that cannot build C is not usable for anything. */
            bool blocks = found.builds_c == 0;
            finding *entry = open_finding(report,
                                          blocks ? finding_error : finding_warning,
                                          section_compilers, blocks, subject);
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

        bool blocks = found.builds_cxx == 0;
        finding *entry = open_finding(report,
                                      blocks ? finding_error : finding_warning,
                                      section_compilers, blocks, subject);
        if (entry == NULL)
            return;
        (void)fs_format_path(entry->location, sizeof entry->location,
                             "%s", chain->cxx_path);
        set_detail(entry, "builds C, and cannot build C++ in any configuration "
                          "pickup knows to offer");
    }
}

/* Set while a report is being built, so the toolchain loop can say where it
   is without threading a parameter through every check. */
typedef struct {
    diagnostics_watch watch;
    void *context;
} progress;

static progress reporting = { 0 };

bool diagnostics_examine(const inventory *list, distro_family family,
                         diagnostics_report *out) {
    if (list->count == 0) {
        finding *entry = open_finding(out, finding_error, section_compilers, true, "compilers");
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

    for (size_t i = 0; i < list->count; i++) {
        /* Announced before the work rather than after it, so the first
           toolchain is not examined in silence. */
        if (reporting.watch != NULL)
            reporting.watch(list->items[i].id, i, list->count, reporting.context);
        survey_toolchain(&list->items[i], &surveys[i]);
    }
    if (reporting.watch != NULL)
        reporting.watch("", list->count, list->count, reporting.context);

    /* Causes before leftovers, so that a toolchain already accounted for by a
       broken GCC is not reported a second time on its own. */
    coverage found = measure(list, surveys);
    summarise_compilers(list, found, out);

    /* After the summary, so a reader sees what the machine has before what
       changed about it. */
    for (size_t i = 0; i < list->count; i++)
        refresh_configuration(&list->items[i], &surveys[i], out);
    check_gcc_installations(list, surveys, family, found, out, explained);
    check_remaining(list, surveys, explained, found, out);

    free(surveys);
    free(explained);
    return true;
}

bool diagnostics_run(diagnostics_report *out) {
    return diagnostics_run_watched(NULL, NULL, out);
}

bool diagnostics_run_watched(diagnostics_watch watch, void *context,
                             diagnostics_report *out) {
    *out = (diagnostics_report){ .count = 0 };
    reporting = (progress){ .watch = watch, .context = context };

    check_environment(out);
    check_home(out);
    check_dev_tools(out);

    inventory list;
    if (!inventory_load(&list, false))
        return false;

    bool examined = diagnostics_examine(&list, distro_detect(), out);
    inventory_free(&list);
    reporting = (progress){ 0 };
    return examined;
}
