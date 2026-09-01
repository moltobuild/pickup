#include <pickup/services/install_service.h>

#include <pickup/detect/capability.h>
#include <pickup/detect/probe.h>
#include <pickup/detect/recipe.h>
#include <pickup/detect/scanner.h>
#include <pickup/services/archive_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/process_service.h>
#include <pickup/util/progress.h>
#include <pickup/util/str_list.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

/*
 * The registry publishes archives whose bin and lib are already at the top, so
 * there is no leading component to drop. That is a property of what it packs
 * rather than a guess about someone else's layout, and it is why nothing here
 * has to know how the archive was assembled.
 */
#define STRIP_NOTHING 0

/* Where a toolchain is assembled before it takes its real name. */
#define PARTIAL_SUFFIX ".partial"

/* Where a prefix keeps its programs. */
#define PREFIX_BIN "bin"

/* What the progress bar says. */
#define DOWNLOAD_LABEL_FORMAT "downloading %s %s"
#define EXTRACT_LABEL_FORMAT "extracting %s %s"
#define VERIFY_LABEL_FORMAT "verifying %s %s"
#define PREPARING_LABEL "preparing the extraction"
#define LABEL_SIZE (REGISTRY_NAME_MAX + REGISTRY_VERSION_MAX + 32)

/* The name a downloaded archive is kept under while it is on its way in. */
#define ARCHIVE_NAME_FORMAT "%s-%s-%s.%s"

static install_report report_of(install_status status) {
    install_report report = {0};
    report.status = status;
    return report;
}

bool install_succeeded(install_status status) { return status == install_ok; }

const char *install_status_message(install_status status) {
    switch (status) {
    case install_ok:
        return "installed";
    case install_no_downloader:
        return "curl is required to download from the registry";
    case install_no_extractor:
        return "tar is required to unpack what the registry packs";
    case install_no_zstd:
        return "this tar cannot open a zstd archive";
    case install_unsupported_format:
        return "the registry packed this in a way this build cannot open";
    case install_yanked:
        return "withdrawn by the registry, and not to be used for new builds";
    case install_download_failed:
        return "the download failed";
    case install_hash_mismatch:
        return "sha256 mismatch, archive discarded";
    case install_extract_failed:
        return "the archive could not be unpacked";
    case install_not_a_toolchain:
        return "the archive holds no usable compiler";
    case install_not_a_tool:
        return "the archive holds no binary that answers";
    case install_path_error:
        return "the pickup home could not be prepared";
    }
    return "unknown failure";
}

/* --- getting the blob --- */

/* Download the artifact into the downloads directory. */
static bool fetch_archive(const registry_artifact *artifact, char *out, size_t out_size) {
    char downloads[PICKUP_PATHS_MAX];
    if (!paths_downloads(downloads, sizeof downloads) || !fs_make_dirs(downloads))
        return false;

    /* Named from the coordinate rather than from the URL: what the blob is
       called is the registry's business, and a name taken from a URL is a name
       taken from something that may contain a path. */
    if (!fs_format_path(out, out_size, "%s/" ARCHIVE_NAME_FORMAT, downloads, artifact->name,
                        artifact->version, artifact->target, artifact->format))
        return false;

    char label[LABEL_SIZE];
    snprintf(label, sizeof label, DOWNLOAD_LABEL_FORMAT, artifact->name, artifact->version);
    return http_download_with_progress(artifact->download_url, out, artifact->size_bytes, label);
}

/* Redraw the verification bar, but only when the figure it shows would change:
   the digest is fed in 64 KB at a time, and a gigabyte of that is thirty
   thousand redraws nobody can read. */
static void watch_verify(long long done, long long total, void *context) {
    const char *label = context;
    static int last_percent = -1;

    if (total <= 0 || !progress_is_interactive(stdout))
        return;

    int percent = (int)((done * 100) / total);
    if (percent == last_percent && done < total)
        return;
    last_percent = percent;
    progress_draw(stdout, label, done, total);
}

/* Compare the archive against the digest the registry published. */
static bool digest_matches(const char *archive, const registry_artifact *artifact,
                           install_report *report) {
    char label[LABEL_SIZE];
    snprintf(label, sizeof label, VERIFY_LABEL_FORMAT, artifact->name, artifact->version);

    bool interactive = progress_is_interactive(stdout);
    bool hashed = sha256_file_watched(archive, report->actual, watch_verify, label);
    if (interactive)
        progress_done(stdout);
    if (!hashed)
        return false;

    (void)fs_format_path(report->expected, sizeof report->expected, "%s", artifact->checksum);
    return sha256_hex_equal(report->actual, artifact->checksum);
}

/* --- assembling it --- */

/* Where a thing is assembled, emptied of anything an interrupted install may
   have left.

   Staged inside the directory it will be renamed into, so the rename that
   finishes the install stays within one filesystem and therefore stays
   atomic. */
static bool prepare_partial(const char *root, char *partial, size_t partial_size) {
    if (!fs_make_dirs(root))
        return false;
    if (!fs_format_path(partial, partial_size, "%s/%s", root, PARTIAL_SUFFIX))
        return false;
    return fs_remove_tree(partial) && fs_make_dirs(partial);
}

static bool extract_to_partial(const char *archive, const registry_artifact *artifact,
                               const char *partial) {
    char label[LABEL_SIZE];
    snprintf(label, sizeof label, EXTRACT_LABEL_FORMAT, artifact->name, artifact->version);

    /* Nothing is selected out of it. The registry publishes what a build uses
       and nothing else, so the choice of what to keep was made when it was
       packed, by something that knew. */
    const archive_request request = {
        .strip_components = STRIP_NOTHING,
        .label = label,
        .waiting_label = PREPARING_LABEL,
    };
    return archive_extract_selected(archive, partial, &request);
}

/* Count what the installed compiler actually compiles. */
static size_t count_proven_features(const toolchain *chain) {
    capability_set proven = capability_probe(chain->path, lang_c);

    size_t total = 0;
    const capability *catalog = capability_catalog(&total);
    size_t count = 0;
    for (size_t i = 0; i < total; i++) {
        if (catalog[i].lang == lang_c && capability_set_has(proven, i))
            count++;
    }
    return count;
}

/*
 * The C drivers a toolchain is identified through, in order of preference.
 *
 * Which one is picked matters beyond appearances: the C++ driver is found by
 * transforming the C driver's name, so `gcc` leads to `g++` and `clang` to
 * `clang++`, while `c++` leads nowhere — it is already the C++ side and has no
 * C counterpart to derive. Identify a toolchain through `c++` and it ends up
 * recorded as having no C++ driver at all.
 */
static const char *const preferred_drivers[] = {"gcc", "clang", "cc"};
#define PREFERRED_DRIVER_COUNT (sizeof preferred_drivers / sizeof preferred_drivers[0])

/* Ask the binary at <bin>/<name> what it is. */
static bool identify_named(const char *bin, const char *name, toolchain *out) {
    char path[PICKUP_PATHS_MAX];
    if (!fs_format_path(path, sizeof path, "%s/%s", bin, name))
        return false;
    return fs_path_exists(path) && probe_identify(path, out);
}

/* Anything else in the directory that names a compiler, for a layout that
   ships none of the usual spellings. Taken as it comes, since at that point
   there is nothing left to prefer. */
static bool identify_by_scanning(const char *bin, toolchain *out) {
    DIR *dir = opendir(bin);
    if (dir == NULL)
        return false;

    bool found = false;
    const struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (scanner_is_candidate_name(entry->d_name))
            found = identify_named(bin, entry->d_name, out);
    }
    closedir(dir);
    return found;
}

/*
 * Find the compiler in a prefix by looking for one, rather than by knowing its
 * name.
 *
 * What a toolchain calls its driver is the publisher's decision: one arrives as
 * `clang`, another only under its target triple as `x86_64-conda-linux-gnu-gcc`.
 * Neither is assumed.
 */
static bool identify_in_prefix(const char *prefix, const char *published, toolchain *out) {
    /* What the recipe named, before anything this file would guess. A cross
       prefix carries a driver under each of these names and only one of them
       emits for the published target: llvm-mingw's `bin/clang` builds for the
       host and its `bin/x86_64-w64-mingw32-clang` for Windows, so a search by
       convention finds the host one and every later step describes a compiler
       nobody published. Tried rather than trusted -- probe_identify still asks
       the binary what it is, and a recipe naming something absent falls through
       to the search below. */
    if (published != NULL && published[0] != '\0') {
        char path[PICKUP_PATHS_MAX];
        if (fs_format_path(path, sizeof path, "%s/%s", prefix, published) && fs_path_exists(path) &&
            probe_identify(path, out))
            return true;
    }

    char bin[PICKUP_PATHS_MAX];
    if (!fs_format_path(bin, sizeof bin, "%s/%s", prefix, PREFIX_BIN))
        return false;

    for (size_t i = 0; i < PREFERRED_DRIVER_COUNT; i++) {
        if (identify_named(bin, preferred_drivers[i], out))
            return true;
    }
    return identify_by_scanning(bin, out);
}

/*
 * The test a tool has to pass before it is adopted.
 *
 * A formatter has nothing to compile, so what stands in for "it builds" is "it
 * answers": the binary is run and asked to identify itself. Same principle as
 * the toolchain check — nothing is adopted for having unpacked — applied to
 * what this kind of thing actually does.
 *
 * `binary` is the path the registry published, relative to the prefix. When it
 * published none, the name it is known by is the reasonable guess.
 */
static bool tool_answers(const char *prefix, const registry_artifact *artifact) {
    char path[PICKUP_PATHS_MAX];
    bool composed =
        artifact->binary[0] != '\0'
            ? fs_format_path(path, sizeof path, "%s/%s", prefix, artifact->binary)
            : fs_format_path(path, sizeof path, "%s/%s/%s", prefix, PREFIX_BIN, artifact->name);
    if (!composed || !fs_path_exists(path))
        return false;

    const char *argv[] = {path, "--version", NULL};
    process_result result = process_try(argv, NULL);
    return result.completed && result.exit_code == 0;
}

/* Move the finished tree to the name it is found under from now on. */
static bool adopt(const char *partial, const toolchain *chain, char *final_path,
                  size_t final_size) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);

    const char *target = chain->target[0] != '\0' ? chain->target : "unknown";
    if (!paths_toolchain_dir(toolchain_vendor_name(chain->vendor), version, target, final_path,
                             final_size))
        return false;

    /* Reinstalling replaces what was there rather than merging into it. */
    if (!fs_remove_tree(final_path))
        return false;
    return fs_rename(partial, final_path);
}

/* The same for a tool, named after what was asked for and the version that
   arrived, so two versions of one tool do not collide. */
static bool adopt_tool(const char *partial, const char *tools, const registry_artifact *artifact,
                       char *final_path, size_t final_size) {
    if (!fs_format_path(final_path, final_size, "%s/%s-%s", tools, artifact->name,
                        artifact->version))
        return false;

    if (!fs_remove_tree(final_path))
        return false;
    return fs_rename(partial, final_path);
}

/*
 * Leave the installed toolchain configured to build.
 *
 * What was unpacked knows how to compile C++ and, on Linux, does not know where
 * to find a C++ standard library until it is told: the one it borrows from the
 * system may be incomplete, and the one it carries needs a run-time search path
 * before the programs it links will start. Installing a compiler and leaving it
 * in that state is delivering half of it.
 *
 * The recipe is discovered against the toolchain in its final location, since a
 * run-time search path written before the move would name a directory that no
 * longer exists. Failure here is not failure of the install: what was installed
 * still works for anything that reads the recipe from `resolve`.
 */
static void configure_installed(install_report *report, const char *published) {
    toolchain chain;
    if (!identify_in_prefix(report->directory, published, &chain))
        return;
    probe_find_cxx_driver(&chain);
    /* Recorded where it now lives, so the report names paths that exist. */
    report->installed = chain;

    /* Only Clang reads a configuration file beside its own binary. A GCC would
       ignore one, so writing it would leave a file that looks like
       configuration and is not; what a caller needs is published by `resolve`,
       which covers both. */
    bool reads_config = chain.vendor == vendor_clang || chain.vendor == vendor_apple_clang;
    if (!reads_config)
        return;

    link_recipe c = recipe_discover(&chain, lang_c);

    if (chain.cxx_path[0] != '\0') {
        link_recipe cxx = recipe_discover(&chain, lang_cxx);
        (void)recipe_write_config(chain.cxx_path, &cxx);

        /* One toolchain stands on one GCC. C++ is the half with a standard
           library to choose, and whichever it chose it still takes the startup
           objects and libgcc from a GCC — so leaving C to go on picking its own
           would have a project's C and C++ objects built against two different
           runtimes. */
        (void)recipe_align_gcc(&cxx, chain.path, &c);
    }
    (void)recipe_write_config(chain.path, &c);
}

/* --- proving it --- */

/*
 * The operating system this pickup runs on, spelled the way a target triple
 * spells it.
 *
 * A guess about somebody else's naming, and treated as one: a host with no name
 * here claims nothing, and every toolchain is then held to the stricter test
 * rather than the looser one.
 */
#if defined(_WIN32)
#define HOST_OS_IN_TRIPLE "windows"
#elif defined(__APPLE__)
#define HOST_OS_IN_TRIPLE "darwin"
#elif defined(__linux__)
#define HOST_OS_IN_TRIPLE "linux"
#else
#define HOST_OS_IN_TRIPLE ""
#endif

/* True if what this toolchain emits is not for this machine. Read off the
   triple the compiler itself answered with, so it describes the toolchain that
   arrived rather than the name the artifact was published under. */
static bool emits_for_elsewhere(const toolchain *chain) {
    return HOST_OS_IN_TRIPLE[0] != '\0' && strstr(chain->target, HOST_OS_IN_TRIPLE) == NULL;
}

/*
 * The test that decides whether what was unpacked is a toolchain at all.
 *
 * Counting features is not enough, and finding that out is what this is for:
 * those probes avoid headers on purpose, so a compiler whose sysroot never
 * arrived passes every one of them and then fails on `#include <stdio.h>`. What
 * was unpacked is only a toolchain once it has compiled, linked and run
 * something.
 *
 * Asked through the recipe rather than bare, because needing flags is not the
 * same as being broken: a compiler carrying its own newer standard library
 * links against it and then cannot start until the loader is told where it is.
 * What matters is that some configuration works, not that the default one does.
 *
 * "Run something" is the one part a cross toolchain cannot do. What it builds
 * is for another machine, so it links and never starts here, and requiring the
 * program to run rejects every cross compiler there is. `resolve` settled this
 * already -- a toolchain asked to emit for elsewhere is judged on whether it
 * linked -- and installing has to answer it the same way, or the two disagree
 * about what a working toolchain is.
 */
static bool proves_it_compiles(const char *prefix, const char *published, install_report *report) {
    if (!identify_in_prefix(prefix, published, &report->installed))
        return false;

    report->features_proven = count_proven_features(&report->installed);
    probe_find_cxx_driver(&report->installed);

    const bool must_run = !emits_for_elsewhere(&report->installed);
    link_recipe c = recipe_discover_for(&report->installed, lang_c, stdlib_unknown, must_run);
    if (report->features_proven == 0 || !c.usable)
        return false;

    /* And C++ too, whenever there is a driver for it. A toolchain published as
       a C and C++ compiler, delivered with a C++ compiler that builds nothing,
       is half a toolchain — and half is the state this check exists to stop
       being reported as success. */
    if (report->installed.cxx_path[0] != '\0') {
        link_recipe cxx =
            recipe_discover_for(&report->installed, lang_cxx, stdlib_unknown, must_run);
        if (!cxx.usable)
            return false;
    }
    return true;
}

/* --- the install --- */

/* What has to be there before anything is downloaded. */
static install_status check_environment(const registry_artifact *artifact) {
    if (!http_available())
        return install_no_downloader;
    if (!archive_available())
        return install_no_extractor;
    /* Compared rather than inferred from the URL: how a blob is packed is
       something the registry states, and a name is not a statement. */
    if (strcmp(artifact->format, REGISTRY_FORMAT_TAR_ZST) != 0)
        return install_unsupported_format;
    if (!archive_supports_zstd())
        return install_no_zstd;
    return install_ok;
}

install_report install_run(const install_request *request) {
    const registry_artifact *artifact = request->artifact;

    install_status ready = check_environment(artifact);
    if (ready != install_ok)
        return report_of(ready);

    /* Refused before the transfer rather than after it: the answer is the same
       either way, and one of them costs a download. */
    if (artifact->yanked && !request->allow_yanked)
        return report_of(install_yanked);

    char archive[PICKUP_PATHS_MAX];
    if (!fetch_archive(artifact, archive, sizeof archive))
        return report_of(install_download_failed);

    install_report report = report_of(install_ok);
    if (!digest_matches(archive, artifact, &report)) {
        remove(archive);
        report.status = install_hash_mismatch;
        return report;
    }

    /* A tool is not a compiler and lives apart from them, because nothing
       resolves against it. */
    bool is_tool = artifact->kind == registry_kind_tool;
    char root[PICKUP_PATHS_MAX];
    bool located = is_tool ? paths_tools(root, sizeof root) : paths_toolchains(root, sizeof root);

    char partial[PICKUP_PATHS_MAX];
    if (!located || !prepare_partial(root, partial, sizeof partial)) {
        remove(archive);
        return report_of(install_path_error);
    }

    if (!extract_to_partial(archive, artifact, partial)) {
        remove(archive);
        (void)fs_remove_tree(partial);
        return report_of(install_extract_failed);
    }

    /* The archive has served its purpose; keeping it around to never be read
       again is not a cache, it is litter. */
    remove(archive);

    if (is_tool) {
        if (!tool_answers(partial, artifact)) {
            (void)fs_remove_tree(partial);
            return report_of(install_not_a_tool);
        }
        (void)fs_tree_size(partial, &report.installed_size);
        if (!adopt_tool(partial, root, artifact, report.directory, sizeof report.directory)) {
            (void)fs_remove_tree(partial);
            return report_of(install_path_error);
        }
        return report;
    }

    /* Proved before it is adopted, so a reinstall that turns out to be broken
       leaves the working one that was already there untouched. */
    if (!proves_it_compiles(partial, request->artifact->c_driver, &report)) {
        (void)fs_remove_tree(partial);
        return report_of(install_not_a_toolchain);
    }

    (void)fs_tree_size(partial, &report.installed_size);
    if (!adopt(partial, &report.installed, report.directory, sizeof report.directory)) {
        (void)fs_remove_tree(partial);
        return report_of(install_path_error);
    }

    /* Done last, and against the final location: a run-time search path
       discovered under `.partial` would name a directory that stops existing
       the moment the rename finishes. The cost is discovering the recipe twice,
       and it is not a saving worth making. */
    configure_installed(&report, request->artifact->c_driver);
    return report;
}
