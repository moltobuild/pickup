#include <pickup/commands/uninstall_command.h>

#include <pickup/commands/probe_progress.h>
#include <pickup/exit_code.h>
#include <pickup/services/cache_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/inventory_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/preference_service.h>
#include <pickup/util/format.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Answers that mean yes. Anything else, including an empty line, does not. */
#define ANSWER_YES       "y"
#define ANSWER_YES_LONG  "yes"

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
    printf("  %s%s%s%s\n", directory,
           size[0] != '\0' ? "  (" : "", size, size[0] != '\0' ? ")" : "");
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

int uninstall_command_run(const char *name, bool assume_yes) {
    if (name == NULL) {
        fprintf(stderr, "pickup: uninstall needs a toolchain name\n");
        return exit_usage_error;
    }

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
        fprintf(stderr, "pickup: '%s' names %zu toolchains; name one exactly\n",
                name, matching);
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
        fprintf(stderr, "pickup: %s is a system toolchain; pickup did not install it\n",
                chain->id);
        fprintf(stderr, "  %s\n", chain->path);
        inventory_free(&list);
        return exit_usage_error;
    }

    char directory[PICKUP_PATHS_MAX];
    if (!paths_owning_toolchain(chain->path, directory, sizeof directory)) {
        fprintf(stderr, "pickup: %s is not inside the toolchains directory\n",
                chain->id);
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
