#include <pickup/commands/list_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>
#include <pickup/util/table.h>

#include <stdio.h>

/* The columns of the human-readable table. */
static const char *const list_headers[] = { "NAME", "VENDOR", "VERSION", "TARGET" };
#define LIST_COLUMNS (sizeof list_headers / sizeof list_headers[0])

/* Room for a formatted version ("12.3.0"). */
#define VERSION_SIZE 32

/* Stands in for a field the probe could not fill. */
#define FIELD_MISSING "-"

/* Lay one toolchain out as cells. `version` backs the version cell, which is
   the only column with no string of its own in the model. */
static void row_of(const toolchain *chain, char *version, size_t version_size,
                   const char **cells) {
    toolchain_version_format(chain->version, version, version_size);
    cells[0] = chain->name;
    cells[1] = toolchain_vendor_name(chain->vendor);
    cells[2] = version;
    cells[3] = chain->target[0] != '\0' ? chain->target : FIELD_MISSING;
}

static void print_text(const inventory *list) {
    if (list->count == 0) {
        printf("No compilers found on PATH.\n");
        return;
    }

    table columns;
    table_init(&columns, list_headers, LIST_COLUMNS);

    for (size_t i = 0; i < list->count; i++) {
        char version[VERSION_SIZE];
        const char *cells[LIST_COLUMNS];
        row_of(&list->items[i], version, sizeof version, cells);
        table_fit_row(&columns, cells);
    }

    table_print_header(&columns, stdout);
    for (size_t i = 0; i < list->count; i++) {
        char version[VERSION_SIZE];
        const char *cells[LIST_COLUMNS];
        row_of(&list->items[i], version, sizeof version, cells);
        table_print_row(&columns, cells, stdout);
    }
}

/* Indexed sections rather than an array of tables: the machine format exists
   to be consumed, and `[[toolchain]]` is a corner of TOML that the readers on
   the other side need not support. `[toolchain.0]` is plain enough for any of
   them. */
static void print_toml(const inventory *list) {
    for (size_t i = 0; i < list->count; i++) {
        const toolchain *chain = &list->items[i];
        char version[32];
        toolchain_version_format(chain->version, version, sizeof version);
        printf("[toolchain.%zu]\n", i);
        printf("name = \"%s\"\n", chain->name);
        printf("path = \"%s\"\n", chain->path);
        printf("vendor = \"%s\"\n", toolchain_vendor_name(chain->vendor));
        printf("version = \"%s\"\n", version);
        printf("target = \"%s\"\n", chain->target);
        printf("cxx_path = \"%s\"\n", chain->cxx_path);
        if (i + 1 < list->count)
            printf("\n");
    }
}

int list_command_run(bool as_toml) {
    inventory list;
    if (!inventory_load(&list, false)) {
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
