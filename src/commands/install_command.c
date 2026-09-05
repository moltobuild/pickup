#include <pickup/commands/install_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/archive_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/install_service.h>
#include <pickup/sources/registry_source.h>
#include <pickup/util/color.h>
#include <pickup/util/format.h>
#include <pickup/util/progress.h>

#include <stdio.h>
#include <string.h>

/* What the spinner says while the registry answers. A spinner rather than a
   bar: the answer's length is never announced, so there is no fraction to
   draw. */
#define FETCH_LABEL "asking the registry"

/* On stderr, for the same reason the report is on stdout: one of them is the
   answer and the other is not. */
static void watch_fetch(size_t frame, void *context) {
    progress_line *line = context;
    if (!progress_is_interactive(stderr))
        return;
    spinner_wait(stderr, FETCH_LABEL, frame);
    line->drawn = true;
}

/* Say where this was going to come from, so a failure names the registry that
   produced it rather than leaving it to be guessed. */
static void report_registry(void) {
    char base[REGISTRY_URL_MAX];
    if (registry_base_url(base, sizeof base))
        fprintf(stderr, "  registry: %s\n", base);
}

/*
 * Nothing is published under that name.
 *
 * Answered from the catalogues rather than from a failed request, so what comes
 * back is a list of what does exist instead of an error that cannot be told
 * apart from the network being down.
 */
static int report_unknown_name(const char *name) {
    fprintf(stderr, "pickup: nothing is published as '%s'\n", name != NULL ? name : "");

    registry_entry_list matches;
    if (registry_search_watched(name, NULL, NULL, &matches) && matches.count > 0) {
        fprintf(stderr, "  did you mean:\n");
        for (size_t i = 0; i < matches.count; i++)
            fprintf(stderr, "    %s (%s %s)\n", matches.items[i].name,
                    registry_kind_name(matches.items[i].kind), matches.items[i].latest_version);
        registry_entry_list_free(&matches);
    } else {
        registry_entry_list_free(&matches);
        report_registry();
    }
    return exit_no_match;
}

/* Report what would be installed, without spending the download. */
static int report_dry_run(const registry_artifact *artifact) {
    char size[FORMAT_SIZE_MAX];
    format_size(artifact->size_bytes, size, sizeof size);

    printf("name     %s\n", artifact->name);
    printf("kind     %s\n", registry_kind_name(artifact->kind));
    printf("version  %s\n", artifact->version);
    printf("target   %s\n", artifact->target);
    printf("format   %s\n", artifact->format);
    printf("size     %s (%lld bytes)\n", size, artifact->size_bytes);
    printf("sha256   %s\n", artifact->checksum);
    printf("url      %s\n", artifact->download_url);
    if (artifact->description[0] != '\0')
        printf("about    %s\n", artifact->description);
    if (artifact->yanked)
        printf("yanked   yes, withdrawn by the registry\n");
    return exit_ok;
}

/* Say what went wrong, and what the user can do about it. */
static int report_failure(const install_report *report, const registry_artifact *artifact) {
    fprintf(stderr, "%s%s%s %s %s: %s\n", color_error(), format_cross(), color_reset(),
            artifact->name, artifact->version, install_status_message(report->status));

    switch (report->status) {
    case install_hash_mismatch:
        /* Both digests, because which one is wrong is the whole question. */
        fprintf(stderr, "  expected  %s\n", report->expected);
        fprintf(stderr, "  actual    %s\n", report->actual);
        break;
    case install_yanked:
        fprintf(stderr, "  nothing was downloaded\n");
        fprintf(stderr,
                "  name the whole version to install it anyway: "
                "pickup install %s --version %s\n",
                artifact->name, artifact->version);
        /* An answer rather than a breakage: the registry replied, and what it
           has is not to be used for new builds. */
        return exit_no_match;
    case install_no_codec: {
        /* Named rather than described: what is missing is a program, and the
           only useful thing to print is which one. */
        const char *needed = install_format_requirement(artifact->format);
        fprintf(stderr, "  this one is packed %s, and this tar cannot open that\n",
                artifact->format);
        if (needed != NULL && archive_delegation_works())
            fprintf(stderr, "  install %s, or use a tar that has it built in\n", needed);
        else if (needed != NULL)
            fprintf(stderr, "  use a tar with %s built in: this one would start %s and hang\n",
                    needed, needed);
        break;
    }
    case install_extract_stalled:
        fprintf(stderr, "  tar produced nothing for long enough to be stopped\n");
        fprintf(stderr, "  a tar that hands %s to another program deadlocks on Windows,\n",
                artifact->format);
        fprintf(stderr, "  where one with the codec built in does not\n");
        break;
    default:
        break;
    }
    return exit_failure;
}

static int report_tool_installed(const install_report *report, const registry_artifact *artifact) {
    char size[FORMAT_SIZE_MAX];
    format_size(report->installed_size, size, sizeof size);

    printf("%s%s%s %s %s installed in %s%s (%s)%s\n", color_ok(), format_check(), color_reset(),
           artifact->name, artifact->version, color_dim(), report->directory, size, color_reset());
    return exit_ok;
}

static int report_toolchain_installed(const install_report *report,
                                      const registry_artifact *artifact) {
    /* What was proven, in the same breath as the claim that it is installed:
       the number is what says the thing that arrived actually compiles. */
    printf("probing installed toolchain%*s%zu features\n", 24, "", report->features_proven);

    char version[32];
    toolchain_version_format(report->installed.version, version, sizeof version);

    char size[FORMAT_SIZE_MAX];
    format_size(report->installed_size, size, sizeof size);

    printf("%s%s%s %s %s installed in %s%s (%s)%s\n", color_ok(), format_check(), color_reset(),
           toolchain_vendor_name(report->installed.vendor), version, color_dim(), report->directory,
           size, color_reset());

    /* What was asked for and what answered are not always spelled the same, so
       say so rather than let it look like the wrong thing was installed. */
    if (strcmp(version, artifact->version) != 0)
        printf("  (published as %s)\n", artifact->version);
    return exit_ok;
}

/*
 * Split "clang@22.1.0" into "clang" and "22.1.0". Returns false when `raw`
 * carries no '@', which is the common case and not an error.
 */
static bool split_versioned_name(const char *raw, char *name, size_t name_size, char *version,
                                 size_t version_size) {
    const char *at = strchr(raw, '@');
    if (at == NULL)
        return false;

    size_t length = (size_t)(at - raw);
    if (length >= name_size)
        length = name_size - 1;
    memcpy(name, raw, length);
    name[length] = '\0';

    snprintf(version, version_size, "%s", at + 1);
    return true;
}

/* Find the one artifact to install, or report why there is none. */
static int choose(const install_command_request *request, const registry_entry *entry,
                  const char *target, registry_artifact *out) {
    registry_artifact_list list;
    progress_line line = {.drawn = false};
    bool fetched =
        registry_fetch_releases_watched(entry->kind, request->name, request->version, target,
                                        request->refresh, watch_fetch, &line, &list);
    progress_line_clear(stderr, &line);

    if (!fetched) {
        fprintf(stderr, "pickup: could not read what the registry publishes for %s\n",
                request->name);
        report_registry();
        registry_artifact_list_free(&list);
        return exit_failure;
    }

    bool chosen = registry_select(&list, request->version, out);
    registry_artifact_list_free(&list);
    if (chosen)
        return exit_ok;

    /* Nothing matched is an answer, and gets the exit code that says so rather
       than the one that means something broke. */
    if (request->version != NULL && request->version[0] != '\0')
        fprintf(stderr, "pickup: no %s matches version %s\n", request->name, request->version);
    else
        fprintf(stderr, "pickup: no %s is published for %s\n", request->name, target);
    report_registry();
    return exit_no_match;
}

int install_command_run(const install_command_request *request) {
    /* "install clang@22.1.0" is "install clang --version 22.1.0" spelled as
        one token; naming the version twice is a conflict, not emphasis. */
    char split_name[REGISTRY_NAME_MAX];
    char split_version[REGISTRY_VERSION_MAX];
    install_command_request resolved = *request;
    if (request->name != NULL && split_versioned_name(request->name, split_name, sizeof split_name,
                                                      split_version, sizeof split_version)) {
        if (request->version != NULL && request->version[0] != '\0') {
            fprintf(stderr, "pickup: '%s' already names a version; --version %s is redundant\n",
                    request->name, request->version);
            return exit_usage_error;
        }
        resolved.name = split_name;
        resolved.version = split_version;
        request = &resolved;
    }
    if (!registry_name_is_simple(request->name)) {
        fprintf(stderr, "pickup: '%s' is not a name the registry could publish\n",
                request->name != NULL ? request->name : "");
        return exit_usage_error;
    }
    if (!http_available()) {
        fprintf(stderr, "pickup: %s is required to reach the registry\n", http_requirement());
        return exit_failure;
    }

    const char *target = registry_host_target();
    if (target[0] == '\0') {
        fprintf(stderr, "pickup: the registry publishes nothing for this machine\n");
        return exit_no_match;
    }

    /* Which catalogue holds it decides where it is installed and what it has to
       prove, so it is settled before anything else. */
    registry_entry entry;
    if (!registry_find(request->name, request->refresh, &entry))
        return report_unknown_name(request->name);

    registry_artifact artifact;
    int found = choose(request, &entry, target, &artifact);
    if (found != exit_ok)
        return found;

    if (request->dry_run)
        return report_dry_run(&artifact);

    /* Withdrawn is allowed through only when the user named the whole version,
       which is the difference between asking for it and being handed it. */
    const install_request install = {
        .artifact = &artifact,
        .allow_yanked = registry_version_is_exact(request->version),
    };
    if (artifact.yanked && install.allow_yanked)
        fprintf(stderr,
                "! %s %s was withdrawn by the registry; "
                "installing it because you named it\n",
                artifact.name, artifact.version);

    install_report report = install_run(&install);
    if (!install_succeeded(report.status))
        return report_failure(&report, &artifact);

    return artifact.kind == registry_kind_tool ? report_tool_installed(&report, &artifact)
                                               : report_toolchain_installed(&report, &artifact);
}
