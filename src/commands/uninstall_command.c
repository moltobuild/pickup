#include <pickup/commands/uninstall_command.h>

#include <pickup/commands/probe_progress.h>
#include <pickup/detect/tools.h>
#include <pickup/exit_code.h>
#include <pickup/services/cache_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/inventory_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/preference_service.h>
#include <pickup/util/format.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Answers that mean yes. Anything else, including an empty line, does not. */
#define ANSWER_YES "y"
#define ANSWER_YES_LONG "yes"

/* Room for the answer. A line longer than this is not one of the above. */
#define ANSWER_SIZE 16

/* Ask before removing, and take silence for no.

   The command names what to remove, so the intention is not in doubt; what the
   prompt guards against is a name that matched something other than what the
   user pictured. It shows the directory and its size for that reason: those
   are what tell the two apart. */
static bool confirmed(const char *id, const char *directory) {
    long long bytes = 0;
    char size[FORMAT_SIZE_MAX] = "";
    if (fs_tree_size(directory, &bytes))
        format_size(bytes, size, sizeof size);

    printf("Remove %s\n", id);
    printf("  %s%s%s%s\n", directory, size[0] != '\0' ? "  (" : "", size,
           size[0] != '\0' ? ")" : "");
    printf("This cannot be undone. Continue? [y/N] ");
    (void)fflush(stdout);

    char answer[ANSWER_SIZE];
    if (fgets(answer, sizeof answer, stdin) == NULL)
        return false;
    answer[strcspn(answer, "\r\n")] = '\0';
    return strcmp(answer, ANSWER_YES) == 0 || strcmp(answer, ANSWER_YES_LONG) == 0;
}

/* Drop the preference when it named the toolchain being removed.

   A default pointing at nothing is not harmless: `resolve` would fall back to
   ranking candidates itself and say why, once per invocation, about a
   toolchain the user already knows they deleted. */
static void forget_if_default(const char *id) {
    char preferred[PREFERENCE_VALUE_MAX];
    if (!preference_default_get(preferred, sizeof preferred))
        return;
    if (strcmp(preferred, id) != 0)
        return;

    if (preference_default_clear())
        printf("It was the default; there is no default now.\n");
    else
        fprintf(stderr, "pickup: removed it, but could not clear the default\n");
}

/* --- tools --- */

/* How many installed versions of one tool are worth listing in a refusal. */
#define TOOL_MATCHES_MAX 16

typedef struct {
    char directories[TOOL_MATCHES_MAX][PICKUP_PATHS_MAX];
    char ids[TOOL_MATCHES_MAX][PICKUP_ID_MAX];
    size_t count; /* every match, including any past what is kept */
} tool_matches;

/* Whether the directory `entry` holds `name`, at `version` when one is given.
   `<name>-` followed by a digit, so that `clang-tidy` does not also mean a
   `clang-tidy-extra` that happens to be installed beside it. */
static bool names_the_tool(const char *entry, const char *name, const char *version) {
    const size_t length = strlen(name);
    if (strncmp(entry, name, length) != 0 || entry[length] != '-')
        return false;
    const char *installed = entry + length + 1;
    if (version != NULL)
        return strcmp(installed, version) == 0;
    return *installed >= '0' && *installed <= '9';
}

/* Every installed version of `name` (just `version` of it, when given). */
static void find_tools(const char *name, const char *version, tool_matches *out) {
    out->count = 0;
    char tools[PICKUP_PATHS_MAX];
    if (!paths_tools(tools, sizeof tools))
        return;
    DIR *dir = opendir(tools);
    if (dir == NULL)
        return;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !names_the_tool(entry->d_name, name, version))
            continue;
        if (out->count < TOOL_MATCHES_MAX) {
            const char *installed = entry->d_name + strlen(name) + 1;
            (void)fs_format_path(out->directories[out->count], PICKUP_PATHS_MAX, "%s/%s", tools,
                                 entry->d_name);
            (void)snprintf(out->ids[out->count], PICKUP_ID_MAX, "%s@%s", name, installed);
        }
        out->count++;
    }
    closedir(dir);
}

/*
 * Remove a tool, when `request` names one pickup installed.
 *
 * `clang-tidy@21.1.8` names one version; `clang-tidy` names whatever is
 * installed, and is refused when that is more than one, for the reason the
 * toolchain side gives. Returns false, touching nothing, when no tool by that
 * name is installed and it is not a tool's name either, so the caller can go
 * on to look for a toolchain.
 */
static bool uninstall_tool(const char *request, bool assume_yes, int *code) {
    char name[PICKUP_ID_MAX];
    (void)snprintf(name, sizeof name, "%s", request);
    char *at = strchr(name, '@');
    const char *version = NULL;
    if (at != NULL) {
        *at = '\0';
        version = at + 1;
    }

    tool_matches matches;
    find_tools(name, version, &matches);
    if (matches.count == 0) {
        /* A toolchain id carries an '@' too ("apple-clang@21.0.0-apple"), so
           only a name pickup knows as a tool is answered here. */
        tool_kind kind;
        if (!tools_kind_of(name, &kind))
            return false;
        if (version != NULL)
            fprintf(stderr, "pickup: %s %s is not installed\n", name, version);
        else
            fprintf(stderr, "pickup: no %s is installed\n", name);
        *code = exit_no_match;
        return true;
    }

    if (matches.count > 1) {
        fprintf(stderr, "pickup: '%s' names %zu installed versions; name one exactly\n", request,
                matches.count);
        for (size_t i = 0; i < matches.count && i < TOOL_MATCHES_MAX; i++)
            fprintf(stderr, "  %s\n", matches.ids[i]);
        *code = exit_usage_error;
        return true;
    }

    if (!assume_yes && isatty(STDIN_FILENO) == 1 &&
        !confirmed(matches.ids[0], matches.directories[0])) {
        printf("Nothing was removed.\n");
        *code = exit_ok;
        return true;
    }
    if (!fs_remove_tree(matches.directories[0])) {
        fprintf(stderr, "pickup: could not remove %s\n", matches.directories[0]);
        *code = exit_failure;
        return true;
    }
    printf("%s Removed %s\n", format_check(), matches.ids[0]);
    *code = exit_ok;
    return true;
}

int uninstall_command_run(const char *name, bool assume_yes) {
    if (name == NULL) {
        fprintf(stderr, "pickup: uninstall needs the name of a toolchain or a tool\n");
        return exit_usage_error;
    }

    /* Tools first: answering for one needs no scan of the machine's
       compilers, and a name that is neither falls through untouched. */
    int code = exit_ok;
    if (uninstall_tool(name, assume_yes, &code))
        return code;

    inventory list;
    if (!probe_progress_load(&list, false)) {
        fprintf(stderr, "pickup: could not scan for compilers\n");
        return exit_failure;
    }

    const toolchain *chain = inventory_find(&list, name);
    if (chain == NULL) {
        fprintf(stderr, "pickup: no toolchain named '%s'\n", name);
        inventory_free(&list);
        return exit_no_match;
    }

    /* Naming one thing and removing another is the failure worth preventing
       here, and a loose query is how it happens. The count comes from the
       whole inventory, so `gcc` on a machine with three of them is refused
       rather than resolved to the newest. */
    size_t matching = inventory_count_matching(&list, name);
    if (matching > 1) {
        fprintf(stderr, "pickup: '%s' names %zu toolchains; name one exactly\n", name, matching);
        for (size_t i = 0; i < list.count; i++) {
            if (toolchain_matches(&list.items[i], name))
                fprintf(stderr, "  %s  (%s)\n", list.items[i].id,
                        toolchain_source_name(list.items[i].source));
        }
        inventory_free(&list);
        return exit_usage_error;
    }

    /* A compiler the package manager owns is not Pickup's to delete, and
       saying which one it is beats a refusal the reader has to interpret. */
    if (chain->source != toolchain_source_pickup) {
        fprintf(stderr, "pickup: %s is a system toolchain; pickup did not install it\n", chain->id);
        fprintf(stderr, "  %s\n", chain->path);
        inventory_free(&list);
        return exit_usage_error;
    }

    char directory[PICKUP_PATHS_MAX];
    if (!paths_owning_toolchain(chain->path, directory, sizeof directory)) {
        fprintf(stderr, "pickup: %s is not inside the toolchains directory\n", chain->id);
        inventory_free(&list);
        return exit_failure;
    }

    char id[PICKUP_ID_MAX];
    snprintf(id, sizeof id, "%s", chain->id);
    inventory_free(&list);

    /* Asked only where there is someone to answer. A pipe cannot say yes, and
       refusing to act without one would break every script that names a
       toolchain outright. */
    if (!assume_yes && isatty(STDIN_FILENO) == 1 && !confirmed(id, directory)) {
        printf("Nothing was removed.\n");
        return exit_ok;
    }

    if (!fs_remove_tree(directory)) {
        fprintf(stderr, "pickup: could not remove %s\n", directory);
        return exit_failure;
    }

    /* The cached inventory still describes a compiler that is gone. Discarding
       it costs one scan; keeping it would have `list` reporting a toolchain
       that cannot be invoked. */
    cache_discard();

    printf("%s Removed %s\n", format_check(), id);
    forget_if_default(id);
    return exit_ok;
}
