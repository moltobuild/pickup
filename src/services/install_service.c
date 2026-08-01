#include <pickup/services/install_service.h>

#include <pickup/detect/probe.h>
#include <pickup/services/archive_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>

#include <stdio.h>

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
#define LABEL_SIZE 64

static install_report report_of(install_status status) {
    install_report report = { 0 };
    report.status = status;
    return report;
}

bool install_succeeded(install_status status) {
    return status == install_ok || status == install_ok_unverified;
}

const char *install_status_message(install_status status) {
    switch (status) {
    case install_ok:              return "installed";
    case install_ok_unverified:   return "installed, integrity not verified";
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

/* Compare the archive against the digest the source published. */
static bool digest_matches(const char *archive, const release_asset *asset,
                           install_report *report) {
    if (!sha256_file(archive, report->actual))
        return false;
    (void)fs_format_path(report->expected, sizeof report->expected, "%s", asset->sha256);
    return sha256_hex_equal(report->actual, asset->sha256);
}

/* Unpack into a directory that does not yet look like an installation. */
static bool extract_to_partial(const char *archive, char *partial, size_t partial_size) {
    char toolchains[PICKUP_PATHS_MAX];
    if (!paths_toolchains(toolchains, sizeof toolchains) || !fs_make_dirs(toolchains))
        return false;
    if (!fs_format_path(partial, partial_size, "%s/%s", toolchains, PARTIAL_SUFFIX))
        return false;

    /* Left over from an install that was interrupted. */
    if (!fs_remove_tree(partial) || !fs_make_dirs(partial))
        return false;
    return archive_extract(archive, partial, STRIP_TOP_LEVEL);
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
    if (!extract_to_partial(archive, partial, sizeof partial)) {
        remove(archive);
        (void)fs_remove_tree(partial);
        return report_of(install_extract_failed);
    }

    if (!identify_installed(partial, &report.installed)) {
        remove(archive);
        (void)fs_remove_tree(partial);
        return report_of(install_not_a_toolchain);
    }

    if (!adopt(partial, &report.installed, report.directory, sizeof report.directory)) {
        remove(archive);
        (void)fs_remove_tree(partial);
        return report_of(install_path_error);
    }

    /* The archive has served its purpose; keeping a gigabyte around to never
       be read again is not a cache, it is litter. */
    remove(archive);

    report.status = verifiable ? install_ok : install_ok_unverified;
    return report;
}
