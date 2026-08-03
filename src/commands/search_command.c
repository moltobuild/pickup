#include <pickup/commands/search_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/http_service.h>
#include <pickup/sources/conda_source.h>
#include <pickup/sources/llvm_source.h>
#include <pickup/util/format.h>
#include <pickup/util/progress.h>
#include <pickup/util/table.h>

#include <stdio.h>
#include <string.h>

/* What can be searched for. The GNU project publishes no binaries of its own,
   so a GCC comes from conda-forge, where it is packaged as `gxx` — the C++
   half, which depends on the C one. */
#define TOOLCHAIN_CLANG "clang"
#define TOOLCHAIN_LLVM  "llvm"
#define TOOLCHAIN_GCC   "gcc"
#define CONDA_GCC_ROOT  "gxx"

static const char *const search_headers[] = { "VERSION", "SIZE", "ASSET" };
#define SEARCH_COLUMNS (sizeof search_headers / sizeof search_headers[0])

/* What one build of a conda package is shown as. */
static const char *const conda_headers[] = { "VERSION", "BUILD", "PACKAGE" };
#define CONDA_COLUMNS (sizeof conda_headers / sizeof conda_headers[0])

static bool is_clang(const char *name) {
    return name != NULL
        && (strcmp(name, TOOLCHAIN_CLANG) == 0 || strcmp(name, TOOLCHAIN_LLVM) == 0);
}

static bool is_gcc(const char *name) {
    return name != NULL && strcmp(name, TOOLCHAIN_GCC) == 0;
}

/* Only the newest build of each version is listed.

   The channel keeps every rebuild, and a dozen lines saying 16.1.0 differing
   in a hash answer a question nobody asked: what is on offer is the version,
   and which build of it to take is the resolver's business. */
static bool version_already_listed(const conda_list *list, size_t upto,
                                   const char *version) {
    for (size_t i = 0; i < upto; i++) {
        if (strcmp(list->items[i].version, version) == 0)
            return true;
    }
    return false;
}

static void print_conda_text(const conda_list *list) {
    table columns;
    table_init(&columns, conda_headers, CONDA_COLUMNS);

    for (size_t i = 0; i < list->count; i++) {
        if (version_already_listed(list, i, list->items[i].version))
            continue;
        const char *cells[CONDA_COLUMNS] = {
            list->items[i].version, list->items[i].build, list->items[i].name
        };
        table_fit_row(&columns, cells);
    }

    table_print_header(&columns, stdout);
    for (size_t i = 0; i < list->count; i++) {
        if (version_already_listed(list, i, list->items[i].version))
            continue;
        const char *cells[CONDA_COLUMNS] = {
            list->items[i].version, list->items[i].build, list->items[i].name
        };
        table_print_row(&columns, cells, stdout);
    }
}

static void print_conda_toml(const conda_list *list) {
    size_t shown = 0;
    for (size_t i = 0; i < list->count; i++) {
        if (version_already_listed(list, i, list->items[i].version))
            continue;
        const conda_package *package = &list->items[i];
        if (shown++ > 0)
            printf("\n");
        printf("[release.%zu]\n", shown - 1);
        printf("version = \"%s\"\n", package->version);
        printf("build = \"%s\"\n", package->build);
        printf("package = \"%s\"\n", package->name);
        printf("url = \"%s\"\n", package->url);
        printf("size = %lld\n", package->size);
    }
}

/* What the spinner says while conda-forge answers. Named for what is actually
   being fetched: a listing of one package's builds, not an index of releases. */
#define CONDA_FETCH_LABEL "fetching the conda-forge listing"

static void watch_conda_fetch(size_t frame, void *context) {
    progress_line *line = context;
    if (!progress_is_interactive(stderr))
        return;
    spinner_wait(stderr, CONDA_FETCH_LABEL, frame);
    line->drawn = true;
}

/* Search conda-forge for the GCCs it publishes for this host. */
static int search_gcc(const search_command_request *request) {
    conda_list list;
    progress_line line = { .drawn = false };
    /* Watched for the same reason the LLVM one is: this is the part that
       touches the network, and a command that says nothing while it waits
       looks like one that has died. */
    bool fetched = conda_fetch_packages_watched(CONDA_GCC_ROOT, request->version,
                                                request->refresh, watch_conda_fetch,
                                                &line, &list);
    progress_line_clear(stderr, &line);

    if (!fetched) {
        fprintf(stderr, "pickup: could not read the conda-forge channel\n");
        conda_list_free(&list);
        return exit_failure;
    }

    if (list.count == 0) {
        if (request->version != NULL && request->version[0] != '\0')
            fprintf(stderr, "pickup: no gcc matches version %s\n", request->version);
        else
            fprintf(stderr, "pickup: no gcc is published for this machine\n");
        conda_list_free(&list);
        return exit_no_match;
    }

    if (request->as_toml)
        print_conda_toml(&list);
    else
        print_conda_text(&list);

    conda_list_free(&list);
    return exit_ok;
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

/* What the spinner says while the index is on its way. A spinner rather than a
   bar: the API never announces how long its answer is, so there is no fraction
   to draw. */
#define FETCH_LABEL "fetching the release index"

/* On stderr, for the same reason the table is on stdout: one of them is the
   answer and the other is not. */
static void watch_fetch(size_t frame, void *context) {
    progress_line *line = context;
    if (!progress_is_interactive(stderr))
        return;
    spinner_wait(stderr, FETCH_LABEL, frame);
    line->drawn = true;
}

int search_command_run(const search_command_request *request) {
    const char *name = request->name;
    if (!is_clang(name) && !is_gcc(name)) {
        fprintf(stderr, "pickup: nothing to search for named '%s'\n",
                name != NULL ? name : "");
        fprintf(stderr, "  try: pickup search %s, or pickup search %s\n",
                TOOLCHAIN_CLANG, TOOLCHAIN_GCC);
        return exit_usage_error;
    }
    if (!http_available()) {
        fprintf(stderr, "pickup: %s is required to reach the release index\n",
                http_requirement());
        return exit_failure;
    }

    if (is_gcc(name))
        return search_gcc(request);

    release_list list;
    progress_line line = { .drawn = false };
    bool fetched = llvm_fetch_releases_watched(request->version, request->refresh,
                                               watch_fetch, &line, &list);
    progress_line_clear(stderr, &line);

    if (!fetched) {
        fprintf(stderr, "pickup: could not read the list of releases\n");
        return exit_failure;
    }

    if (list.count == 0) {
        /* Nothing matched is an answer, and gets the exit code that says so
           rather than the one that means something broke. */
        if (request->version != NULL && request->version[0] != '\0')
            fprintf(stderr, "pickup: no %s release matches version %s\n",
                    name, request->version);
        else
            fprintf(stderr, "pickup: no %s release ships a build for this machine\n", name);
        release_list_free(&list);
        return exit_no_match;
    }

    if (request->as_toml)
        print_toml(&list);
    else
        print_text(&list);

    release_list_free(&list);
    return exit_ok;
}
