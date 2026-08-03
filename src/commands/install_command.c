#include <pickup/commands/install_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/install_service.h>
#include <pickup/sources/conda_closure.h>
#include <pickup/sources/llvm_source.h>
#include <pickup/util/color.h>
#include <pickup/util/format.h>
#include <pickup/util/progress.h>

#include <stdio.h>
#include <string.h>

#define TOOLCHAIN_CLANG "clang"
#define TOOLCHAIN_LLVM  "llvm"
#define TOOLCHAIN_GCC   "gcc"

/*
 * What is asked of conda-forge for a GCC.
 *
 * `gxx` rather than `gcc`, because `gcc` there is the C compiler alone and a
 * toolchain that cannot build C++ is half a toolchain. `gxx` depends on `gcc`,
 * so asking for the larger of the two brings both, along with the libstdc++ a
 * C++ program needs to compile against.
 */
#define CONDA_GCC_ROOT "gxx"

/* The flag that lets an unverifiable release through. */
#define ALLOW_UNVERIFIED_FLAG "--allow-unverified"

static bool is_clang(const char *name) {
    return name != NULL
        && (strcmp(name, TOOLCHAIN_CLANG) == 0 || strcmp(name, TOOLCHAIN_LLVM) == 0);
}

static bool is_gcc(const char *name) {
    return name != NULL && strcmp(name, TOOLCHAIN_GCC) == 0;
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
    fprintf(stderr, "%s%s%s %s %s: %s\n", color_error(), format_cross(), color_reset(),
            TOOLCHAIN_CLANG, asset->version, install_status_message(report->status));

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
    /* What was proven, in the same breath as the claim that it is installed:
       the number is what says the pruning did not quietly break it. */
    printf("probing installed toolchain%*s%zu features\n",
           24, "", report->features_proven);

    if (report->status == install_ok_unverified)
        printf("! not verified: the release publishes no digest\n");
    if (report->status == install_ok_unpruned)
        printf("! the minimal profile did not compile, extracted in full\n");

    char version[32];
    toolchain_version_format(report->installed.version, version, sizeof version);

    char size[FORMAT_SIZE_MAX];
    format_size(report->installed_size, size, sizeof size);

    printf("%s%s%s %s %s installed in %s%s (%s)%s\n",
           color_ok(), format_check(), color_reset(),
           toolchain_vendor_name(report->installed.vendor), version,
           color_dim(), report->directory, size, color_reset());

    /* What was asked for and what answered are not always spelled the same,
       so say so rather than let it look like the wrong thing was installed. */
    if (strcmp(version, asset->version) != 0)
        printf("  (published as %s)\n", asset->version);
    return exit_ok;
}

/* What a closure would install, without spending the download. */
static int report_closure_dry_run(const conda_closure *closure) {
    char total[FORMAT_SIZE_MAX];
    format_size(conda_closure_size(closure), total, sizeof total);
    printf("packages %zu\n", closure->packages.count);
    printf("size     %s\n", total);

    for (size_t i = 0; i < closure->packages.count; i++) {
        const conda_package *package = &closure->packages.items[i];
        char size[FORMAT_SIZE_MAX];
        format_size(package->size, size, sizeof size);
        printf("  %-32s %-12s %-20s %8s\n", package->name, package->version,
               package->build, size);
    }
    return exit_ok;
}

/* What the spinner says while the closure is worked out: which package is
   being asked about, and how many are settled. More useful than a bare
   spinner, and there is nothing to measure a bar against. */
#define RESOLVE_LABEL_FORMAT "resolving %s"
#define RESOLVE_LABEL_SIZE (CONDA_NAME_MAX + 32)

static void watch_resolve(const char *name, size_t resolved, void *context) {
    progress_line *line = context;
    if (!progress_is_interactive(stderr))
        return;

    char label[RESOLVE_LABEL_SIZE];
    snprintf(label, sizeof label, RESOLVE_LABEL_FORMAT, name);
    spinner_wait(stderr, label, resolved);
    line->drawn = true;
}

/* Install a GCC, which arrives from conda-forge as a set of packages. */
static int install_gcc(const install_command_request *request) {
    conda_closure closure;
    progress_line line = { .drawn = false };
    bool resolved = conda_resolve_watched(CONDA_GCC_ROOT, request->version,
                                          request->refresh, watch_resolve,
                                          &line, &closure);
    progress_line_clear(stderr, &line);

    if (!resolved) {
        fprintf(stderr, "pickup: could not read the conda-forge channel\n");
        conda_closure_free(&closure);
        return exit_failure;
    }

    /* An incomplete closure names what stopped it. Installing most of a
       toolchain would be worse than installing none of it. */
    if (!closure.complete) {
        fprintf(stderr, "pickup: no gcc satisfies the request\n");
        fprintf(stderr, "  missing: %s", closure.missing);
        if (closure.required_by[0] != '\0')
            fprintf(stderr, " (required by %s)", closure.required_by);
        fprintf(stderr, "\n  nothing was downloaded\n");
        conda_closure_free(&closure);
        return exit_no_match;
    }

    if (request->dry_run) {
        int code = report_closure_dry_run(&closure);
        conda_closure_free(&closure);
        return code;
    }

    install_report report = install_run_closure(&closure, request->allow_unverified);
    conda_closure_free(&closure);

    if (!install_succeeded(report.status)) {
        fprintf(stderr, "%s%s%s %s: %s\n", color_error(), format_cross(), color_reset(),
                TOOLCHAIN_GCC, install_status_message(report.status));
        if (report.status == install_unverifiable)
            fprintf(stderr, "  re-run with %s to install it anyway\n",
                    ALLOW_UNVERIFIED_FLAG);
        if (report.status == install_hash_mismatch) {
            fprintf(stderr, "  expected  %s\n", report.expected);
            fprintf(stderr, "  actual    %s\n", report.actual);
        }
        return exit_failure;
    }

    char version[32];
    toolchain_version_format(report.installed.version, version, sizeof version);
    char size[FORMAT_SIZE_MAX];
    format_size(report.installed_size, size, sizeof size);

    printf("probing installed toolchain%*s%zu features\n",
           24, "", report.features_proven);
    if (report.status == install_ok_unverified)
        printf("! not verified: the channel publishes no digest for every package\n");
    printf("%s%s%s %s %s installed in %s%s (%s)%s\n",
           color_ok(), format_check(), color_reset(),
           toolchain_vendor_name(report.installed.vendor), version,
           color_dim(), report.directory, size, color_reset());
    return exit_ok;
}

int install_command_run(const install_command_request *request) {
    if (is_gcc(request->name))
        return install_gcc(request);

    if (!is_clang(request->name)) {
        fprintf(stderr, "pickup: nothing to install named '%s'\n",
                request->name != NULL ? request->name : "");
        fprintf(stderr, "  try: pickup install %s, or pickup install %s\n",
                TOOLCHAIN_CLANG, TOOLCHAIN_GCC);
        return exit_usage_error;
    }

    release_list list;
    if (!llvm_fetch_releases(request->version, request->refresh, &list)) {
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
        .full = request->full,
    };
    install_report report = install_run(&install);

    if (!install_succeeded(report.status))
        return report_failure(&report, &asset);
    return report_success(&report, &asset);
}
