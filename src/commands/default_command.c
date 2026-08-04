#include <pickup/commands/default_command.h>

#include <pickup/commands/probe_progress.h>
#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>
#include <pickup/services/preference_service.h>
#include <pickup/util/format.h>

#include <stdio.h>
#include <string.h>

/* Told to the reader when nothing is preferred, which is the state every
   machine starts in rather than a fault. */
#define NO_DEFAULT_TEXT "No default toolchain; resolve ranks candidates itself.\n"

static int report_clear(void) {
    if (!preference_default_clear()) {
        fprintf(stderr, "pickup: could not clear the default\n");
        return exit_failure;
    }
    printf("%s Cleared the default toolchain\n", format_check());
    return exit_ok;
}

static void print_toml(const char *id, bool installed) {
    printf("[default]\n");
    printf("id = \"%s\"\n", id != NULL ? id : "");
    /* Always emitted, so a consumer reads one shape whether or not anything is
       set: a missing key and an empty one are the same answer here. */
    printf("installed = %s\n", installed ? "true" : "false");
}

/*
 * Report what is preferred, and whether it is still there.
 *
 * The stored preference is an identity, not a path, so it survives the
 * toolchain being reinstalled. The price is that it can outlive the toolchain
 * itself — a directory removed by hand leaves the name behind — and a report
 * that only echoed the name would keep insisting on a compiler that is gone.
 */
static int report_current(bool as_toml) {
    char id[PREFERENCE_VALUE_MAX];
    if (!preference_default_get(id, sizeof id)) {
        if (as_toml)
            print_toml(NULL, false);
        else
            printf(NO_DEFAULT_TEXT);
        return exit_ok;
    }

    inventory list;
    if (!probe_progress_load(&list, false)) {
        fprintf(stderr, "pickup: could not scan for compilers\n");
        return exit_failure;
    }
    const toolchain *chain = inventory_find(&list, id);
    bool installed = chain != NULL;

    if (as_toml) {
        print_toml(id, installed);
    } else if (installed) {
        printf("%s\n", id);
        printf("  %s\n", chain->path);
    } else {
        printf("%s (no longer installed)\n", id);
        printf("  Set another, or clear it with --clear.\n");
    }
    inventory_free(&list);

    /* A preference naming nothing is a state a script must be able to act on,
       and it is the same kind of answer as a resolve that matched nothing. */
    return installed ? exit_ok : exit_no_match;
}

/*
 * Say which one a loose query landed on.
 *
 * `pickup default gcc` is a reasonable thing to type and the newest is a
 * reasonable reading of it, so unlike `uninstall` this is not refused: nothing
 * is destroyed by preferring the wrong compiler, and it is one command to
 * change. What must not happen is the choice going unmentioned, since the
 * identity that gets stored is the resolved one and not what was typed.
 */
static void note_if_loose(const inventory *list, const char *query,
                          const toolchain *chosen) {
    size_t matching = inventory_count_matching(list, query);
    if (matching < 2)
        return;
    fprintf(stderr, "pickup: '%s' names %zu toolchains; chose %s\n",
            query, matching, chosen->id);
}

static int set_default(const char *name, bool as_toml) {
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
    note_if_loose(&list, name, chain);

    /* The resolved identity, never the query. `gcc@latest` stored as typed
       would change meaning under the user the next time anything is installed,
       which is the opposite of what pinning a default is for. */
    char id[PICKUP_ID_MAX];
    snprintf(id, sizeof id, "%s", chain->id);
    inventory_free(&list);

    if (!preference_default_set(id)) {
        fprintf(stderr, "pickup: could not store the default\n");
        return exit_failure;
    }

    if (as_toml)
        print_toml(id, true);
    else
        printf("%s Default toolchain is now %s\n", format_check(), id);
    return exit_ok;
}

int default_command_run(const default_command_request *request) {
    if (request->clear && request->name != NULL) {
        fprintf(stderr, "pickup: --clear takes no toolchain name\n");
        return exit_usage_error;
    }
    if (request->clear)
        return report_clear();
    if (request->name == NULL)
        return report_current(request->as_toml);
    return set_default(request->name, request->as_toml);
}
