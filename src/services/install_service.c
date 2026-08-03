#include <pickup/services/install_service.h>

#include <pickup/detect/capability.h>
#include <pickup/detect/probe.h>
#include <pickup/detect/recipe.h>
#include <pickup/detect/scanner.h>
#include <pickup/services/archive_service.h>
#include <pickup/services/conda_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/util/progress.h>
#include <pickup/util/str_list.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

/* The archives carry one top-level directory named after the release; dropping
   it puts bin and lib straight into the toolchain directory, which is where
   the scanner expects to find them. */
#define STRIP_TOP_LEVEL 1

/* Where a toolchain is assembled before it takes its real name. */
#define PARTIAL_SUFFIX ".partial"

/* The compiler a freshly unpacked LLVM is identified by. */
#define INSTALLED_DRIVER "bin/clang"

/* What the progress bar says while downloading. */
#define DOWNLOAD_LABEL_FORMAT "downloading clang %s"
#define EXTRACT_LABEL_FORMAT  "extracting clang %s"
#define VERIFY_LABEL_FORMAT   "verifying clang %s"
#define PREPARING_LABEL       "preparing the extraction"
#define LABEL_SIZE 64

static install_report report_of(install_status status) {
    install_report report = { 0 };
    report.status = status;
    return report;
}

bool install_succeeded(install_status status) {
    return status == install_ok || status == install_ok_unverified
        || status == install_ok_unpruned;
}

const char *install_status_message(install_status status) {
    switch (status) {
    case install_ok:              return "installed";
    case install_ok_unverified:   return "installed, integrity not verified";
    case install_ok_unpruned:     return "installed in full: the minimal profile did not compile";
    case install_no_downloader:   return "curl is required to download toolchains";
    case install_no_extractor:    return "tar is required to unpack toolchains";
    case install_unverifiable:    return "this release publishes no sha256 digest";
    case install_download_failed: return "the download failed";
    case install_hash_mismatch:   return "sha256 mismatch, archive discarded";
    case install_extract_failed:  return "the archive could not be unpacked";
    case install_not_a_toolchain: return "the archive holds no usable compiler";
    case install_path_error:      return "the pickup home could not be prepared";
    }
    return "unknown failure";
}

/* Download the asset into the downloads directory. */
static bool fetch_archive(const release_asset *asset, char *out, size_t out_size) {
    char downloads[PICKUP_PATHS_MAX];
    if (!paths_downloads(downloads, sizeof downloads) || !fs_make_dirs(downloads))
        return false;
    if (!fs_format_path(out, out_size, "%s/%s", downloads, asset->asset))
        return false;

    char label[LABEL_SIZE];
    snprintf(label, sizeof label, DOWNLOAD_LABEL_FORMAT, asset->version);
    return http_download_with_progress(asset->url, out, asset->size, label);
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

/* Compare the archive against the digest the source published. */
static bool digest_matches(const char *archive, const release_asset *asset,
                           install_report *report) {
    char label[LABEL_SIZE];
    snprintf(label, sizeof label, VERIFY_LABEL_FORMAT, asset->version);

    bool interactive = progress_is_interactive(stdout);
    bool hashed = sha256_file_watched(archive, report->actual, watch_verify, label);
    if (interactive)
        progress_done(stdout);
    if (!hashed)
        return false;

    (void)fs_format_path(report->expected, sizeof report->expected, "%s", asset->sha256);
    return sha256_hex_equal(report->actual, asset->sha256);
}

/* Where a toolchain is assembled, emptied of anything an interrupted install
   may have left. */
static bool prepare_partial(char *partial, size_t partial_size) {
    char toolchains[PICKUP_PATHS_MAX];
    if (!paths_toolchains(toolchains, sizeof toolchains) || !fs_make_dirs(toolchains))
        return false;
    if (!fs_format_path(partial, partial_size, "%s/%s", toolchains, PARTIAL_SUFFIX))
        return false;
    return fs_remove_tree(partial) && fs_make_dirs(partial);
}

/* Unpack, either the profile or the whole release. */
static bool extract_to_partial(const char *archive, const release_asset *asset,
                               bool full, const char *partial) {
    char label[LABEL_SIZE];
    snprintf(label, sizeof label, EXTRACT_LABEL_FORMAT, asset->version);

    archive_request request = {
        .strip_components = STRIP_TOP_LEVEL,
        .label = label,
        .waiting_label = PREPARING_LABEL,
    };
    if (!full) {
        request.patterns = llvm_minimal_patterns(&request.pattern_count);
        request.excludes = llvm_minimal_excludes(&request.exclude_count);
    }
    return archive_extract_selected(archive, partial, &request);
}

/* Count what the installed compiler actually compiles.

   This is the check that makes pruning safe to attempt: the profile is a guess
   about somebody else's layout, and a guess that dropped something essential
   shows up here as a compiler that proves nothing. Leaving one feature behind
   would be missed, but leaving out the builtin headers, the runtime or the
   binary itself cannot be. */
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

/* Ask what was unpacked to identify itself, which also proves it runs. */
static bool identify_installed(const char *partial, toolchain *out) {
    char driver[PICKUP_PATHS_MAX];
    if (!fs_format_path(driver, sizeof driver, "%s/%s", partial, INSTALLED_DRIVER))
        return false;
    if (!fs_path_exists(driver))
        return false;
    return probe_identify(driver, out);
}

/* Move the finished tree to the name it is found under from now on. */
static bool adopt(const char *partial, const toolchain *chain,
                  char *final_path, size_t final_size) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);

    const char *target = chain->target[0] != '\0' ? chain->target : "unknown";
    if (!paths_toolchain_dir(toolchain_vendor_name(chain->vendor), version, target,
                             final_path, final_size))
        return false;

    /* Reinstalling replaces what was there rather than merging into it. */
    if (!fs_remove_tree(final_path))
        return false;
    return fs_rename(partial, final_path);
}

/* --- a toolchain that arrives as a set of packages --- */

/* Defined below, and used by both kinds of install: whatever was unpacked is
   left configured to build before it is reported as installed. */
static void configure_installed(install_report *report);

/* Where the compiler of a conda prefix lives. */
#define CONDA_BIN "bin"

/* What the progress bar says while a package comes down. Sized for the longest
   package name the channel publishes rather than for the ones seen so far. */
#define PACKAGE_LABEL_FORMAT "downloading %s %s"
#define PACKAGE_LABEL_SIZE (CONDA_NAME_MAX + CONDA_VERSION_MAX + 32)

/*
 * The C drivers a toolchain is identified through, in order of preference.
 *
 * Which one is picked matters beyond appearances: the C++ driver is found by
 * transforming the C driver's name, so `gcc` leads to `g++` and `clang` to
 * `clang++`, while `c++` leads nowhere — it is already the C++ side and has no
 * C counterpart to derive. Identify a toolchain through `c++` and it ends up
 * recorded as having no C++ driver at all.
 */
static const char *const preferred_drivers[] = { "gcc", "clang", "cc" };
#define PREFERRED_DRIVER_COUNT \
    (sizeof preferred_drivers / sizeof preferred_drivers[0])

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
 * A conda toolchain installs its driver under the target triple —
 * `x86_64-conda-linux-gnu-gcc` — and a plain `gcc` beside it only when a
 * particular package happened to be part of the closure. Both are things the
 * channel decides, so neither is assumed.
 */
static bool identify_in_prefix(const char *prefix, toolchain *out) {
    char bin[PICKUP_PATHS_MAX];
    if (!fs_format_path(bin, sizeof bin, "%s/%s", prefix, CONDA_BIN))
        return false;

    for (size_t i = 0; i < PREFERRED_DRIVER_COUNT; i++) {
        if (identify_named(bin, preferred_drivers[i], out))
            return true;
    }
    return identify_by_scanning(bin, out);
}

/* Bring one package down and check it against the digest the channel
   published for it. */
static bool fetch_and_verify(conda_package *package, bool allow_unverified,
                             char *out, size_t out_size, install_report *report) {
    char downloads[PICKUP_PATHS_MAX];
    if (!paths_downloads(downloads, sizeof downloads) || !fs_make_dirs(downloads))
        return false;
    if (!fs_format_path(out, out_size, "%s/%s", downloads, package->file))
        return false;

    /* Asked for before the transfer, so a package that cannot be verified is
       refused without spending the bandwidth first. */
    bool verifiable = conda_fetch_sha256(package);
    if (!verifiable && !allow_unverified)
        return false;

    char label[PACKAGE_LABEL_SIZE];
    snprintf(label, sizeof label, PACKAGE_LABEL_FORMAT, package->name, package->version);
    if (!http_download_with_progress(package->url, out, package->size, label))
        return false;

    if (!verifiable)
        return true;

    char actual[SHA256_HEX_SIZE];
    if (!sha256_file(out, actual) || !sha256_hex_equal(actual, package->sha256)) {
        (void)fs_format_path(report->expected, sizeof report->expected, "%s",
                             package->sha256);
        (void)fs_format_path(report->actual, sizeof report->actual, "%s", actual);
        remove(out);
        return false;
    }
    return true;
}

install_report install_run_closure(const conda_closure *closure, bool allow_unverified) {
    if (!http_available())
        return report_of(install_no_downloader);
    if (!archive_available())
        return report_of(install_no_extractor);
    if (closure->packages.count == 0)
        return report_of(install_not_a_toolchain);

    char partial[PICKUP_PATHS_MAX];
    if (!prepare_partial(partial, sizeof partial))
        return report_of(install_path_error);

    install_report report = report_of(install_ok);
    bool verified_all = true;

    for (size_t i = 0; i < closure->packages.count; i++) {
        conda_package package = closure->packages.items[i];

        char archive[PICKUP_PATHS_MAX];
        if (!fetch_and_verify(&package, allow_unverified, archive,
                              sizeof archive, &report)) {
            (void)fs_remove_tree(partial);
            /* A digest that was published and did not match is a different
               failure from one that was never published at all. */
            return report_of(report.actual[0] != '\0' ? install_hash_mismatch
                             : (package.sha256[0] == '\0' ? install_unverifiable
                                                          : install_download_failed));
        }
        verified_all = verified_all && package.sha256[0] != '\0';

        /* Every package over the same prefix, which is what lets the parts of
           a compiler find each other, and the build prefix rewritten out of
           each as it lands. */
        bool unpacked = conda_install_package(archive, partial);
        remove(archive);
        if (!unpacked) {
            (void)fs_remove_tree(partial);
            return report_of(install_extract_failed);
        }
    }

    if (!identify_in_prefix(partial, &report.installed)) {
        (void)fs_remove_tree(partial);
        return report_of(install_not_a_toolchain);
    }
    report.features_proven = count_proven_features(&report.installed);
    probe_find_cxx_driver(&report.installed);

    /*
     * The test that decides whether this is a toolchain at all.
     *
     * Counting features is not enough here, and finding that out is what this
     * check is for: those probes avoid headers on purpose, so a compiler whose
     * sysroot never arrived, or whose embedded paths were rewritten wrongly,
     * passes every one of them and then fails on `#include <stdio.h>`. A set
     * of packages is only a toolchain once it has compiled, linked and run
     * something.
     *
     * Asked through the recipe rather than bare, because needing flags is not
     * the same as being broken: a compiler carrying its own newer standard
     * library links against it and then cannot start until the loader is told
     * where it is. What matters is that some configuration works, not that the
     * default one does.
     */
    link_recipe c = recipe_discover(&report.installed, lang_c);
    if (report.features_proven == 0 || !c.usable) {
        (void)fs_remove_tree(partial);
        return report_of(install_not_a_toolchain);
    }

    /* And C++ too, whenever there is a driver for it. A closure asked for
       because it carries a C++ compiler, delivered with a C++ compiler that
       builds nothing, is half a toolchain — and half is the state this whole
       check exists to stop being reported as success. */
    if (report.installed.cxx_path[0] != '\0') {
        link_recipe cxx = recipe_discover(&report.installed, lang_cxx);
        if (!cxx.usable) {
            (void)fs_remove_tree(partial);
            return report_of(install_not_a_toolchain);
        }
    }

    (void)fs_tree_size(partial, &report.installed_size);
    if (!adopt(partial, &report.installed, report.directory, sizeof report.directory)) {
        (void)fs_remove_tree(partial);
        return report_of(install_path_error);
    }

    configure_installed(&report);
    report.status = verified_all ? install_ok : install_ok_unverified;
    return report;
}

/*
 * Leave the installed toolchain configured to build.
 *
 * What was unpacked knows how to compile C++ and, on Linux, does not know
 * where to find a C++ standard library until it is told: the one it borrows
 * from the system may be incomplete, and the one it carries needs a run-time
 * search path before the programs it links will start. Installing a compiler
 * and leaving it in that state is delivering half of it.
 *
 * The recipe is discovered against the toolchain in its final location, since
 * a run-time search path written before the move would name a directory that
 * no longer exists. Failure here is not failure of the install: what was
 * installed still works for anything that reads the recipe from `resolve`.
 */
static void configure_installed(install_report *report) {
    char driver[PICKUP_PATHS_MAX];
    if (!fs_format_path(driver, sizeof driver, "%s/%s",
                        report->directory, INSTALLED_DRIVER))
        return;

    toolchain chain;
    if (!probe_identify(driver, &chain))
        return;
    probe_find_cxx_driver(&chain);
    /* Recorded where it now lives, so the report names paths that exist. */
    report->installed = chain;

    /* Only Clang reads a configuration file beside its own binary. A GCC would
       ignore one, so writing it would leave a file that looks like
       configuration and is not; what a caller needs is published by `resolve`,
       which covers both. */
    bool reads_config = chain.vendor == vendor_clang
                     || chain.vendor == vendor_apple_clang;
    if (!reads_config)
        return;

    link_recipe c = recipe_discover(&chain, lang_c);

    if (chain.cxx_path[0] != '\0') {
        link_recipe cxx = recipe_discover(&chain, lang_cxx);
        (void)recipe_write_config(chain.cxx_path, &cxx);

        /* One toolchain stands on one GCC. C++ is what forces the choice —
           it is the half that needs a libstdc++ — and leaving C to go on
           picking its own would have a project's C and C++ objects built
           against two different runtimes. */
        (void)recipe_align_gcc(&cxx, chain.path, &c);
    }
    (void)recipe_write_config(chain.path, &c);
}

install_report install_run(const install_request *request) {
    const release_asset *asset = request->asset;

    if (!http_available())
        return report_of(install_no_downloader);
    if (!archive_available())
        return report_of(install_no_extractor);

    /* Decided before anything is downloaded: refusing after a gigabyte would
       waste the transfer to reach the same answer. */
    bool verifiable = asset->sha256[0] != '\0';
    if (!verifiable && !request->allow_unverified)
        return report_of(install_unverifiable);

    char archive[PICKUP_PATHS_MAX];
    if (!fetch_archive(asset, archive, sizeof archive))
        return report_of(install_download_failed);

    install_report report = report_of(install_ok);
    if (verifiable && !digest_matches(archive, asset, &report)) {
        remove(archive);
        report.status = install_hash_mismatch;
        return report;
    }

    char partial[PICKUP_PATHS_MAX];
    if (!prepare_partial(partial, sizeof partial)) {
        remove(archive);
        return report_of(install_path_error);
    }
    /* Unpack the profile, then ask the result to compile. Both a profile that
       fits nothing in this archive and one that dropped something essential
       come out the same way here — as a toolchain that proves nothing — and get
       the same answer: unpack the release whole rather than leave something
       installed that cannot build. */
    bool pruned = !request->full;
    if (pruned) {
        if (extract_to_partial(archive, asset, false, partial)
            && identify_installed(partial, &report.installed))
            report.features_proven = count_proven_features(&report.installed);
        if (report.features_proven == 0)
            pruned = false;
    }

    if (!pruned) {
        if (!prepare_partial(partial, sizeof partial)
            || !extract_to_partial(archive, asset, true, partial)) {
            remove(archive);
            (void)fs_remove_tree(partial);
            return report_of(install_extract_failed);
        }
        report.features_proven = identify_installed(partial, &report.installed)
            ? count_proven_features(&report.installed) : 0;
    }

    if (report.features_proven == 0) {
        remove(archive);
        (void)fs_remove_tree(partial);
        return report_of(install_not_a_toolchain);
    }

    (void)fs_tree_size(partial, &report.installed_size);

    if (!adopt(partial, &report.installed, report.directory, sizeof report.directory)) {
        remove(archive);
        (void)fs_remove_tree(partial);
        return report_of(install_path_error);
    }

    /* The archive has served its purpose; keeping a gigabyte around to never
       be read again is not a cache, it is litter. */
    remove(archive);

    /* Done last, and against the final location: a toolchain that cannot
       build C++ until someone works out the flags is not installed, only
       unpacked. */
    configure_installed(&report);

    /* Three outcomes worth telling apart: it was verified and pruned, it could
       not be verified, or pruning did not hold and the whole release is on
       disk. Each is something the user may want to act on. */
    if (!verifiable)
        report.status = install_ok_unverified;
    else if (!request->full && !pruned)
        report.status = install_ok_unpruned;
    else
        report.status = install_ok;
    return report;
}
