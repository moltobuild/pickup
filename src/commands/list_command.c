#include <pickup/commands/list_command.h>

#include <pickup/commands/probe_progress.h>
#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>
#include <pickup/services/preference_service.h>
#include <pickup/util/table.h>

#include <stdio.h>
#include <string.h>

/* The columns of the human-readable table.

   SOURCE comes last, and says who is responsible for the toolchain rather than
   what it is: `pickup` for one Pickup installed and can remove again, `system`
   for one that was already on the machine and belongs to its package
   manager. */
static const char *const list_headers[] = {
    "NAME", "VENDOR", "VERSION", "TARGET", "SOURCE"
};
#define LIST_COLUMNS (sizeof list_headers / sizeof list_headers[0])

/* Room for a formatted version ("12.3.0"). */
#define VERSION_SIZE 32

/* Stands in for a field the probe could not fill. */
#define FIELD_MISSING "-"

/* How the user's default is marked: on its own row, the way `git branch`
   marks the current branch, rather than as a column that would be blank for
   every toolchain but one. */
#define DEFAULT_MARK " (default)"

/* Room for an identity carrying that mark. */
#define NAME_CELL_SIZE (PICKUP_ID_MAX + sizeof DEFAULT_MARK)

/* Lay one toolchain out as cells. `name` and `version` back the two cells with
   no string of their own in the model. `preferred` is the stored default, or
   NULL when there is none. */
static void row_of(const toolchain *chain, const char *preferred,
                   char *name, size_t name_size,
                   char *version, size_t version_size, const char **cells) {
    toolchain_version_format(chain->version, version, version_size);
    /* The identity, not the basename of whichever link was found first: one
       compiler answering to four names was four rows saying one thing. */
    snprintf(name, name_size, "%s%s", chain->id,
             preferred != NULL && strcmp(chain->id, preferred) == 0
                 ? DEFAULT_MARK : "");

    cells[0] = name;
    cells[1] = toolchain_vendor_name(chain->vendor);
    cells[2] = version;
    cells[3] = chain->target[0] != '\0' ? chain->target : FIELD_MISSING;
    cells[4] = toolchain_source_name(chain->source);
}

static void print_text(const inventory *list) {
    if (list->count == 0) {
        printf("No compilers found on PATH.\n");
        return;
    }

    char preferred[PREFERENCE_VALUE_MAX];
    const char *marked = preference_default_get(preferred, sizeof preferred)
        ? preferred : NULL;

    table columns;
    table_init(&columns, list_headers, LIST_COLUMNS);

    for (size_t i = 0; i < list->count; i++) {
        char name[NAME_CELL_SIZE];
        char version[VERSION_SIZE];
        const char *cells[LIST_COLUMNS];
        row_of(&list->items[i], marked, name, sizeof name,
               version, sizeof version, cells);
        table_fit_row(&columns, cells);
    }

    table_print_header(&columns, stdout);
    for (size_t i = 0; i < list->count; i++) {
        char name[NAME_CELL_SIZE];
        char version[VERSION_SIZE];
        const char *cells[LIST_COLUMNS];
        row_of(&list->items[i], marked, name, sizeof name,
               version, sizeof version, cells);
        table_print_row(&columns, cells, stdout);
    }
}

/* Indexed sections rather than an array of tables: the machine format exists
   to be consumed, and `[[toolchain]]` is a corner of TOML that the readers on
   the other side need not support. `[toolchain.0]` is plain enough for any of
   them. */
static void print_toml(const inventory *list) {
    char preferred[PREFERENCE_VALUE_MAX];
    const char *marked = preference_default_get(preferred, sizeof preferred)
        ? preferred : NULL;

    for (size_t i = 0; i < list->count; i++) {
        const toolchain *chain = &list->items[i];
        char version[32];
        toolchain_version_format(chain->version, version, sizeof version);
        printf("[toolchain.%zu]\n", i);
        /* The identity first, because it is what another command is given to
           name this toolchain again. `name` stays as the binary's own, which
           is still what a build line invokes. */
        printf("id = \"%s\"\n", chain->id);
        printf("name = \"%s\"\n", chain->name);
        printf("path = \"%s\"\n", chain->path);
        printf("vendor = \"%s\"\n", toolchain_vendor_name(chain->vendor));
        printf("version = \"%s\"\n", version);
        printf("target = \"%s\"\n", chain->target);
        printf("source = \"%s\"\n", toolchain_source_name(chain->source));
        printf("cxx_path = \"%s\"\n", chain->cxx_path);
        /* On every entry rather than only the preferred one, so a consumer
           reads the same keys for each and needs no special case. */
        printf("default = %s\n",
               marked != NULL && strcmp(chain->id, marked) == 0 ? "true" : "false");
        if (i + 1 < list->count)
            printf("\n");
    }
}

int list_command_run(bool as_toml) {
    inventory list;
    if (!probe_progress_load(&list, false)) {
        fprintf(stderr, "pickup: could not scan for compilers\n");
        return exit_failure;
    }
    if (as_toml)
        print_toml(&list);
    else
        print_text(&list);
    inventory_free(&list);
    return exit_ok;
}
