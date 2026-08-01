#include <pickup/commands/search_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/http_service.h>
#include <pickup/sources/llvm_source.h>
#include <pickup/util/format.h>
#include <pickup/util/table.h>

#include <stdio.h>
#include <string.h>

/* What can be searched for today. GCC publishes no binaries of its own, so
   there is nothing to offer for it yet. */
#define TOOLCHAIN_CLANG "clang"
#define TOOLCHAIN_LLVM  "llvm"

static const char *const search_headers[] = { "VERSION", "SIZE", "ASSET" };
#define SEARCH_COLUMNS (sizeof search_headers / sizeof search_headers[0])

static bool is_supported(const char *name) {
    return name != NULL
        && (strcmp(name, TOOLCHAIN_CLANG) == 0 || strcmp(name, TOOLCHAIN_LLVM) == 0);
}

static void print_text(const release_list *list) {
    table columns;
    table_init(&columns, search_headers, SEARCH_COLUMNS);

    /* Measured before anything is printed, so the columns fit the longest
       asset name rather than a guess. */
    for (size_t i = 0; i < list->count; i++) {
        char size[FORMAT_SIZE_MAX];
        format_size(list->items[i].size, size, sizeof size);
        const char *cells[SEARCH_COLUMNS] = {
            list->items[i].version, size, list->items[i].asset
        };
        table_fit_row(&columns, cells);
    }

    table_print_header(&columns, stdout);
    for (size_t i = 0; i < list->count; i++) {
        char size[FORMAT_SIZE_MAX];
        format_size(list->items[i].size, size, sizeof size);
        const char *cells[SEARCH_COLUMNS] = {
            list->items[i].version, size, list->items[i].asset
        };
        table_print_row(&columns, cells, stdout);
    }
}

static void print_toml(const release_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        const release_asset *asset = &list->items[i];
        printf("[release.%zu]\n", i);
        printf("version = \"%s\"\n", asset->version);
        printf("asset = \"%s\"\n", asset->asset);
        printf("url = \"%s\"\n", asset->url);
        printf("sha256 = \"%s\"\n", asset->sha256);
        printf("size = %lld\n", asset->size);
        if (i + 1 < list->count)
            printf("\n");
    }
}

int search_command_run(const char *name, const char *version, bool as_toml) {
    if (!is_supported(name)) {
        fprintf(stderr, "pickup: nothing to search for named '%s'\n",
                name != NULL ? name : "");
        fprintf(stderr, "  try: pickup search %s\n", TOOLCHAIN_CLANG);
        return exit_usage_error;
    }
    if (!http_available()) {
        fprintf(stderr, "pickup: %s is required to reach the release index\n",
                http_requirement());
        return exit_failure;
    }

    release_list list;
    if (!llvm_fetch_releases(version, &list)) {
        fprintf(stderr, "pickup: could not read the list of releases\n");
        return exit_failure;
    }

    if (list.count == 0) {
        /* Nothing matched is an answer, and gets the exit code that says so
           rather than the one that means something broke. */
        if (version != NULL && version[0] != '\0')
            fprintf(stderr, "pickup: no %s release matches version %s\n", name, version);
        else
            fprintf(stderr, "pickup: no %s release ships a build for this machine\n", name);
        release_list_free(&list);
        return exit_no_match;
    }

    if (as_toml)
        print_toml(&list);
    else
        print_text(&list);

    release_list_free(&list);
    return exit_ok;
}
