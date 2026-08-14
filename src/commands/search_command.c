#include <pickup/commands/search_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/http_service.h>
#include <pickup/sources/registry_source.h>
#include <pickup/util/format.h>
#include <pickup/util/progress.h>
#include <pickup/util/table.h>

#include <stdio.h>
#include <string.h>

/* A catalogue listing: what exists, and the newest of each. */
static const char *const catalogue_headers[] = {
    "NAME", "KIND", "VERSION", "TARGETS", "PUBLISHED BY",
};
#define CATALOGUE_COLUMNS (sizeof catalogue_headers / sizeof catalogue_headers[0])

/* One name's releases: what can be installed, and whether it should be. The
   author comes before the status because status is empty on most rows, and a
   blank column in the middle of a table reads as a missing value. */
static const char *const release_headers[] = {
    "VERSION", "SIZE", "TARGET", "PUBLISHED BY", "STATUS",
};
#define RELEASE_COLUMNS (sizeof release_headers / sizeof release_headers[0])

/* What a withdrawn release is marked with. Still listed: it remains
   resolvable, and hiding it would leave an existing install unexplained. */
#define STATUS_YANKED "yanked"
#define STATUS_NONE   ""

/* What stands in for an author the registry does not know, because the artifact
   was published before it had accounts. */
#define PUBLISHER_UNKNOWN "-"

/* A cell for an author, so an unknown one reads as unknown rather than as a
   column that failed to print. */
static const char *publisher_cell(const char *published_by) {
    return published_by[0] != '\0' ? published_by : PUBLISHER_UNKNOWN;
}

/* A TOML key is written only when there is something to write: an empty string
   would claim the author is the empty string, and the truth is that nobody
   knows who it was. */
static void print_optional_key(const char *key, const char *value) {
    if (value[0] != '\0')
        printf("%s = \"%s\"\n", key, value);
}

/* Room for the joined target names of one catalogue row. */
#define TARGETS_CELL_MAX (REGISTRY_TARGETS_MAX * (REGISTRY_TARGET_MAX + 2))

/* What the spinner says while the registry answers. A spinner rather than a
   bar: the answer's length is never announced, so there is no fraction to
   draw. */
#define FETCH_LABEL "asking the registry"

/* On stderr, for the same reason the table is on stdout: one of them is the
   answer and the other is not. */
static void watch_fetch(size_t frame, void *context) {
    progress_line *line = context;
    if (!progress_is_interactive(stderr))
        return;
    spinner_wait(stderr, FETCH_LABEL, frame);
    line->drawn = true;
}

static void report_registry(void) {
    char base[REGISTRY_URL_MAX];
    if (registry_base_url(base, sizeof base))
        fprintf(stderr, "  registry: %s\n", base);
}

/* The targets of one row, joined for a cell. */
static void join_targets(const registry_entry *entry, char *out, size_t out_size) {
    out[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < entry->target_count; i++) {
        int written = snprintf(out + used, out_size - used, "%s%s",
                               used > 0 ? ", " : "", entry->targets[i]);
        if (written < 0 || (size_t)written >= out_size - used)
            return;
        used += (size_t)written;
    }
}

/* --- what exists --- */

static void print_catalogue_text(const registry_entry_list *list) {
    table columns;
    table_init(&columns, catalogue_headers, CATALOGUE_COLUMNS);

    /* Measured before anything is printed, so the columns fit the longest name
       rather than a guess. */
    for (size_t pass = 0; pass < 2; pass++) {
        if (pass == 1)
            table_print_header(&columns, stdout);
        for (size_t i = 0; i < list->count; i++) {
            char targets[TARGETS_CELL_MAX];
            join_targets(&list->items[i], targets, sizeof targets);
            const char *cells[CATALOGUE_COLUMNS] = {
                list->items[i].name, registry_kind_name(list->items[i].kind),
                list->items[i].latest_version, targets,
                publisher_cell(list->items[i].published_by),
            };
            if (pass == 0)
                table_fit_row(&columns, cells);
            else
                table_print_row(&columns, cells, stdout);
        }
    }
}

static void print_catalogue_toml(const registry_entry_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        const registry_entry *entry = &list->items[i];
        if (i > 0)
            printf("\n");
        printf("[entry.%zu]\n", i);
        printf("name = \"%s\"\n", entry->name);
        printf("kind = \"%s\"\n", registry_kind_name(entry->kind));
        printf("latest_version = \"%s\"\n", entry->latest_version);
        printf("versions = %zu\n", entry->versions);
        printf("targets = [");
        for (size_t j = 0; j < entry->target_count; j++)
            printf("%s\"%s\"", j > 0 ? ", " : "", entry->targets[j]);
        printf("]\n");
        print_optional_key("published_by", entry->published_by);
    }
}

/*
 * Everything published, across the catalogues.
 *
 * Gathered into one list rather than printed one catalogue at a time, so the
 * columns are measured against every row and a toolchain and a tool line up
 * under the same headings. What the registry separates by kind is, to someone
 * asking what they can install, one list.
 */
static int search_everything(const search_command_request *request) {
    static const registry_kind kinds[] = { registry_kind_toolchain, registry_kind_tool };

    registry_entry_list all;
    registry_entry_list_init(&all);
    progress_line line = { .drawn = false };

    bool any = false;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        registry_entry_list list;
        if (!registry_fetch_catalogue_watched(kinds[i], request->refresh,
                                              watch_fetch, &line, &list)) {
            registry_entry_list_free(&list);
            continue;
        }
        any = true;
        for (size_t j = 0; j < list.count; j++) {
            if (!registry_entry_list_push(&all, &list.items[j]))
                break;
        }
        registry_entry_list_free(&list);
    }
    progress_line_clear(stderr, &line);

    if (!any) {
        registry_entry_list_free(&all);
        fprintf(stderr, "pickup: could not read what the registry publishes\n");
        report_registry();
        return exit_failure;
    }

    if (request->as_toml)
        print_catalogue_toml(&all);
    else
        print_catalogue_text(&all);

    registry_entry_list_free(&all);
    return exit_ok;
}

/* --- one name's releases --- */

static void print_releases_text(const registry_artifact_list *list) {
    table columns;
    table_init(&columns, release_headers, RELEASE_COLUMNS);

    for (size_t pass = 0; pass < 2; pass++) {
        if (pass == 1)
            table_print_header(&columns, stdout);
        for (size_t i = 0; i < list->count; i++) {
            char size[FORMAT_SIZE_MAX];
            format_size(list->items[i].size_bytes, size, sizeof size);
            const char *cells[RELEASE_COLUMNS] = {
                list->items[i].version, size, list->items[i].target,
                publisher_cell(list->items[i].published_by),
                list->items[i].yanked ? STATUS_YANKED : STATUS_NONE,
            };
            if (pass == 0)
                table_fit_row(&columns, cells);
            else
                table_print_row(&columns, cells, stdout);
        }
    }
}

static void print_releases_toml(const registry_artifact_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        const registry_artifact *artifact = &list->items[i];
        if (i > 0)
            printf("\n");
        printf("[release.%zu]\n", i);
        printf("name = \"%s\"\n", artifact->name);
        printf("kind = \"%s\"\n", registry_kind_name(artifact->kind));
        printf("version = \"%s\"\n", artifact->version);
        printf("target = \"%s\"\n", artifact->target);
        printf("format = \"%s\"\n", artifact->format);
        printf("sha256 = \"%s\"\n", artifact->checksum);
        printf("size = %lld\n", artifact->size_bytes);
        printf("yanked = %s\n", artifact->yanked ? "true" : "false");
        print_optional_key("published_by", artifact->published_by);
        printf("url = \"%s\"\n", artifact->download_url);
    }
}

/* What the registry matches for a name it does not publish. Never a failure:
   the answer is a list of what does exist. */
static int suggest(const char *name) {
    registry_entry_list matches;
    if (!registry_search_watched(name, NULL, NULL, &matches) || matches.count == 0) {
        registry_entry_list_free(&matches);
        fprintf(stderr, "pickup: nothing is published as '%s'\n", name);
        report_registry();
        return exit_no_match;
    }

    fprintf(stderr, "pickup: nothing is published as '%s'; these match:\n", name);
    print_catalogue_text(&matches);
    registry_entry_list_free(&matches);
    return exit_no_match;
}

static int search_one(const search_command_request *request) {
    registry_entry entry;
    if (!registry_find(request->name, request->refresh, &entry))
        return suggest(request->name);

    const char *target = registry_host_target();
    if (target[0] == '\0') {
        fprintf(stderr, "pickup: the registry publishes nothing for this machine\n");
        return exit_no_match;
    }

    registry_artifact_list list;
    progress_line line = { .drawn = false };
    bool fetched = registry_fetch_releases_watched(entry.kind, request->name,
                                                   request->version, target,
                                                   request->refresh, watch_fetch,
                                                   &line, &list);
    progress_line_clear(stderr, &line);

    if (!fetched) {
        fprintf(stderr, "pickup: could not read what the registry publishes for %s\n",
                request->name);
        report_registry();
        registry_artifact_list_free(&list);
        return exit_failure;
    }

    if (list.count == 0) {
        if (request->version != NULL && request->version[0] != '\0')
            fprintf(stderr, "pickup: no %s matches version %s\n",
                    request->name, request->version);
        else
            fprintf(stderr, "pickup: no %s is published for %s\n",
                    request->name, target);
        registry_artifact_list_free(&list);
        return exit_no_match;
    }

    if (request->as_toml)
        print_releases_toml(&list);
    else
        print_releases_text(&list);

    registry_artifact_list_free(&list);
    return exit_ok;
}

int search_command_run(const search_command_request *request) {
    if (!http_available()) {
        fprintf(stderr, "pickup: %s is required to reach the registry\n",
                http_requirement());
        return exit_failure;
    }

    if (request->name == NULL || request->name[0] == '\0')
        return search_everything(request);

    if (!registry_name_is_simple(request->name)) {
        fprintf(stderr, "pickup: '%s' is not a name the registry could publish\n",
                request->name);
        return exit_usage_error;
    }
    return search_one(request);
}
