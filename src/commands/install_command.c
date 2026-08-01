#include <pickup/commands/install_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/install_service.h>
#include <pickup/sources/llvm_source.h>
#include <pickup/util/format.h>

#include <stdio.h>
#include <string.h>

#define TOOLCHAIN_CLANG "clang"
#define TOOLCHAIN_LLVM  "llvm"

/* The flag that lets an unverifiable release through. */
#define ALLOW_UNVERIFIED_FLAG "--allow-unverified"

static bool is_supported(const char *name) {
    return name != NULL
        && (strcmp(name, TOOLCHAIN_CLANG) == 0 || strcmp(name, TOOLCHAIN_LLVM) == 0);
}

/* Report what would be installed, without spending the download. */
static int report_dry_run(const release_asset *asset) {
    char size[FORMAT_SIZE_MAX];
    format_size(asset->size, size, sizeof size);

    printf("version  %s\n", asset->version);
    printf("asset    %s\n", asset->asset);
    printf("url      %s\n", asset->url);
    printf("size     %s (%lld bytes)\n", size, asset->size);
    printf("sha256   %s\n", asset->sha256[0] != '\0' ? asset->sha256
                                                     : "(none published)");
    return exit_ok;
}

/* Say what went wrong, and what the user can do about it. */
static int report_failure(const install_report *report, const release_asset *asset) {
    fprintf(stderr, "%s %s %s: %s\n", format_cross(), TOOLCHAIN_CLANG, asset->version,
            install_status_message(report->status));

    switch (report->status) {
    case install_unverifiable:
        fprintf(stderr, "  nothing was downloaded\n");
        fprintf(stderr, "  re-run with %s to install it anyway\n", ALLOW_UNVERIFIED_FLAG);
        break;
    case install_hash_mismatch:
        /* Both digests, because which one is wrong is the whole question. */
        fprintf(stderr, "  expected  %s\n", report->expected);
        fprintf(stderr, "  actual    %s\n", report->actual);
        break;
    default:
        break;
    }
    return exit_failure;
}

static int report_success(const install_report *report, const release_asset *asset) {
    if (report->status == install_ok_unverified)
        printf("! not verified: the release publishes no digest\n");

    char version[32];
    toolchain_version_format(report->installed.version, version, sizeof version);
    printf("%s %s %s installed in %s\n", format_check(),
           toolchain_vendor_name(report->installed.vendor), version, report->directory);

    /* What was asked for and what answered are not always spelled the same,
       so say so rather than let it look like the wrong thing was installed. */
    if (strcmp(version, asset->version) != 0)
        printf("  (published as %s)\n", asset->version);
    return exit_ok;
}

int install_command_run(const install_command_request *request) {
    if (!is_supported(request->name)) {
        fprintf(stderr, "pickup: nothing to install named '%s'\n",
                request->name != NULL ? request->name : "");
        fprintf(stderr, "  try: pickup install %s\n", TOOLCHAIN_CLANG);
        return exit_usage_error;
    }

    release_list list;
    if (!llvm_fetch_releases(request->version, &list)) {
        fprintf(stderr, "pickup: could not read the list of releases\n");
        return exit_failure;
    }

    release_asset asset;
    if (!llvm_best(&list, &asset)) {
        if (request->version != NULL && request->version[0] != '\0')
            fprintf(stderr, "pickup: no %s release matches version %s\n",
                    request->name, request->version);
        else
            fprintf(stderr, "pickup: no %s release ships a build for this machine\n",
                    request->name);
        release_list_free(&list);
        return exit_no_match;
    }
    release_list_free(&list);

    if (request->dry_run)
        return report_dry_run(&asset);

    const install_request install = {
        .asset = &asset,
        .allow_unverified = request->allow_unverified,
    };
    install_report report = install_run(&install);

    if (!install_succeeded(report.status))
        return report_failure(&report, &asset);
    return report_success(&report, &asset);
}
