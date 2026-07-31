#include <pickup/commands/list_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>

#include <stdio.h>

/* Column widths of the human-readable table. */
#define COLUMN_NAME    16
#define COLUMN_VENDOR  12
#define COLUMN_VERSION 10

static void print_text(const inventory *list) {
    if (list->count == 0) {
        printf("No compilers found on PATH.\n");
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        const toolchain *chain = &list->items[i];
        char version[32];
        toolchain_version_format(chain->version, version, sizeof version);
        printf("%-*s %-*s %-*s %s\n",
               COLUMN_NAME, chain->name,
               COLUMN_VENDOR, toolchain_vendor_name(chain->vendor),
               COLUMN_VERSION, version,
               chain->target[0] != '\0' ? chain->target : "-");
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
