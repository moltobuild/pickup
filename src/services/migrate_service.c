#include <pickup/services/migrate_service.h>

#include <pickup/detect/recipe.h>
#include <pickup/services/fs_service.h>
#include <pickup/util/str_list.h>

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

/* rmdir, which removes a directory only when it is empty: the one way of
   deleting the legacy directory that cannot take anything with it. */
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

/* What the legacy directory held, by name. */
#define LEGACY_TOOLCHAINS "toolchains"
#define LEGACY_TOOLS "tools"
#define LEGACY_CONFIG "config.toml"
#define LEGACY_CACHE "cache"
#define LEGACY_DOWNLOADS "downloads"

/* An install interrupted half way, staged under toolchains/ or tools/. It was
   never a toolchain, and the next install would have thrown it away anyway. */
#define LEGACY_PARTIAL ".partial"

/* Finder leaves one of these in any directory it has shown. Keeping the legacy
   directory alive for it would be keeping it alive for nothing. */
#define FINDER_LITTER ".DS_Store"

/* How deep under a moved toolchain a driver's .cfg is looked for: `install`
   writes one beside each driver, which is bin/ or a cross toolchain's
   <triple>/bin. Deeper is a tree of headers that holds none. */
#define CONFIG_SEARCH_DEPTH 2

/* One toolchain that moved: where it was, by the name it was reached through
   and by its canonical name, and where it is now. A .cfg may hold either of
   the first two, because what was written depended on whether the path it came
   from had been resolved. */
typedef struct {
    char from[PICKUP_PATHS_MAX];
    char from_real[PICKUP_PATHS_MAX];
    char to[PICKUP_PATHS_MAX];
} relocation;

typedef struct {
    relocation *items;
    size_t count;
    size_t capacity;
} relocation_list;

static bool relocation_push(relocation_list *list, const relocation *item) {
    if (list->count == list->capacity) {
        const size_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        relocation *grown = realloc(list->items, capacity * sizeof *grown);
        if (grown == NULL)
            return false;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = *item;
    return true;
}

static void note_left(migrate_report *report, const char *relative) {
    if (report->left_count < MIGRATE_LEFT_MAX)
        (void)fs_format_path(report->left[report->left_count], MIGRATE_NAME_MAX, "%s", relative);
    report->left_count++;
}

/* The names in `directory`, read in full before anything in it is renamed:
   readdir over a directory that is being emptied may skip or repeat entries. */
static bool list_entries(const char *directory, str_list *out) {
    str_list_init(out);
    DIR *dir = opendir(directory);
    if (dir == NULL)
        return false;
    bool ok = true;
    const struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        ok = str_list_push(out, entry->d_name);
    }
    closedir(dir);
    return ok;
}

/*
 * Rename each entry of <legacy>/<subdir> into `destination`, one directory at
 * a time, so every toolchain is either entirely in the old place or entirely
 * in the new one.
 *
 * Never over something already there: a name taken at the destination is
 * either the same thing installed again or something else entirely, and in
 * neither case is it the migration's to replace. A rename that fails — the
 * destination on another filesystem, most likely — leaves the entry where it
 * was. Both are reported as left behind.
 */
static size_t move_entries(const char *legacy, const char *subdir, const char *destination,
                           migrate_report *report, relocation_list *moved) {
    char source[PICKUP_PATHS_MAX];
    if (!fs_format_path(source, sizeof source, "%s/%s", legacy, subdir) || !fs_is_dir(source))
        return 0;

    str_list names;
    if (!list_entries(source, &names)) {
        str_list_free(&names);
        note_left(report, subdir);
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < str_list_count(&names); i++) {
        const char *name = str_list_get(&names, i);
        char relative[PICKUP_PATHS_MAX];
        relocation item = {0};
        if (!fs_format_path(relative, sizeof relative, "%s/%s", subdir, name) ||
            !fs_format_path(item.from, sizeof item.from, "%s/%s", source, name) ||
            !fs_format_path(item.to, sizeof item.to, "%s/%s", destination, name)) {
            note_left(report, subdir);
            continue;
        }

        if (strcmp(name, LEGACY_PARTIAL) == 0 || strcmp(name, FINDER_LITTER) == 0) {
            (void)fs_remove_tree(item.from);
            continue;
        }
        if (fs_path_exists(item.to)) {
            note_left(report, relative);
            continue;
        }

        /* Resolved while it still exists: afterwards there is nothing left to
           resolve, and the canonical spelling is one a .cfg may hold. */
        if (!fs_real_path(item.from, item.from_real, sizeof item.from_real))
            item.from_real[0] = '\0';

        if (!fs_make_dirs(destination) || !fs_rename(item.from, item.to)) {
            /* Gone without this process moving it: another pickup started at
               the same moment got there first, and it is where it should be. */
            if (fs_path_exists(item.from))
                note_left(report, relative);
            continue;
        }
        count++;
        if (moved != NULL)
            (void)relocation_push(moved, &item);
    }
    str_list_free(&names);
    return count;
}

/* True when `name` ends in the suffix a driver's configuration file carries. */
static bool is_config_file(const char *name) {
    const size_t length = strlen(name);
    const size_t suffix = sizeof RECIPE_CONFIG_SUFFIX - 1;
    return length > suffix && strcmp(name + length - suffix, RECIPE_CONFIG_SUFFIX) == 0;
}

/* Point every .cfg under `directory` at where the toolchains now are. Links are
   not followed: a toolchain's own tree is all that is Pickup's to edit. */
static void relocate_configs(const char *directory, const relocation_list *moved, int depth) {
    if (depth > CONFIG_SEARCH_DEPTH)
        return;
    str_list names;
    if (!list_entries(directory, &names)) {
        str_list_free(&names);
        return;
    }
    for (size_t i = 0; i < str_list_count(&names); i++) {
        char path[PICKUP_PATHS_MAX];
        if (!fs_format_path(path, sizeof path, "%s/%s", directory, str_list_get(&names, i)))
            continue;
        if (fs_is_dir_no_follow(path)) {
            relocate_configs(path, moved, depth + 1);
            continue;
        }
        if (!is_config_file(path))
            continue;

        /* Every move, not only this toolchain's: a Clang may stand on a GCC
           Pickup installed beside it, and that one moved too. */
        for (size_t m = 0; m < moved->count; m++) {
            const relocation *item = &moved->items[m];
            (void)recipe_relocate_config(path, item->from, item->to);
            if (item->from_real[0] != '\0')
                (void)recipe_relocate_config(path, item->from_real, item->to);
        }
    }
    str_list_free(&names);
}

/* The preferences, unless the new place already has some: those were written
   by this version, which makes them the newer decision. */
static void move_preferences(const char *legacy, const char *config, migrate_report *report) {
    char from[PICKUP_PATHS_MAX];
    char to[PICKUP_PATHS_MAX];
    if (!fs_format_path(from, sizeof from, "%s/%s", legacy, LEGACY_CONFIG) || !fs_path_exists(from))
        return;
    if (!fs_format_path(to, sizeof to, "%s/%s", config, LEGACY_CONFIG) || fs_path_exists(to) ||
        !fs_make_dirs(config) || !fs_rename(from, to)) {
        if (fs_path_exists(from))
            note_left(report, LEGACY_CONFIG);
        return;
    }
    report->preferences_moved = true;
}

/* Whatever else is in the legacy directory, which nothing told this version
   about. It is not Pickup's to delete, so it is left, and named. */
static void note_the_rest(const char *legacy, migrate_report *report) {
    str_list names;
    if (!list_entries(legacy, &names)) {
        str_list_free(&names);
        return;
    }
    for (size_t i = 0; i < str_list_count(&names); i++) {
        const char *name = str_list_get(&names, i);
        /* Already accounted for, entry by entry, or about to be written. */
        if (strcmp(name, LEGACY_TOOLCHAINS) == 0 || strcmp(name, LEGACY_TOOLS) == 0 ||
            strcmp(name, LEGACY_CONFIG) == 0 || strcmp(name, MIGRATE_MARKER_FILENAME) == 0)
            continue;
        if (strcmp(name, FINDER_LITTER) == 0) {
            char path[PICKUP_PATHS_MAX];
            if (fs_format_path(path, sizeof path, "%s/%s", legacy, name))
                (void)remove(path);
            continue;
        }
        note_left(report, name);
    }
    str_list_free(&names);
}

/* The note that stops the next run from trying again, and says why. */
static void leave_marker(const migrate_report *report) {
    char path[PICKUP_PATHS_MAX];
    if (!fs_format_path(path, sizeof path, "%s/%s", report->legacy, MIGRATE_MARKER_FILENAME))
        return;
    char text[3 * PICKUP_PATHS_MAX];
    const int written =
        snprintf(text, sizeof text,
                 "pickup no longer keeps its files here. It moved what it could to\n"
                 "  %s (toolchains and tools)\n"
                 "  %s (preferences)\n"
                 "What is still in this directory could not be moved: its name was already\n"
                 "taken at the destination, or the destination is on another filesystem.\n"
                 "Move it yourself, or delete this file to have pickup try again.\n",
                 report->data, report->config);
    if (written > 0 && (size_t)written < sizeof text)
        (void)fs_write_file(path, text);
}

/* Remove `relative` under the legacy directory if it is empty, and nothing
   else: rmdir refuses a directory with anything still in it. */
static void remove_if_empty(const char *legacy, const char *relative) {
    char path[PICKUP_PATHS_MAX];
    if (fs_format_path(path, sizeof path, "%s/%s", legacy, relative))
        (void)rmdir(path);
}

migrate_report migrate_legacy_home(void) {
    migrate_report report = {0};
    if (paths_relocated())
        return report;

    char legacy[PICKUP_PATHS_MAX];
    if (!paths_legacy_home(legacy, sizeof legacy) || !fs_is_dir(legacy))
        return report;

    char marker[PICKUP_PATHS_MAX];
    if (!fs_format_path(marker, sizeof marker, "%s/%s", legacy, MIGRATE_MARKER_FILENAME) ||
        fs_path_exists(marker))
        return report;

    char toolchains[PICKUP_PATHS_MAX];
    char tools[PICKUP_PATHS_MAX];
    if (!paths_data(report.data, sizeof report.data) ||
        !paths_config(report.config, sizeof report.config) ||
        !paths_toolchains(toolchains, sizeof toolchains) || !paths_tools(tools, sizeof tools))
        return report;
    (void)fs_format_path(report.legacy, sizeof report.legacy, "%s", legacy);
    report.attempted = true;

    relocation_list moved = {0};
    report.toolchains_moved = move_entries(legacy, LEGACY_TOOLCHAINS, toolchains, &report, &moved);
    report.tools_moved = move_entries(legacy, LEGACY_TOOLS, tools, &report, NULL);

    /* After every move, so a .cfg naming a toolchain that moved later in the
       loop is rewritten as well. */
    for (size_t i = 0; i < moved.count; i++)
        relocate_configs(moved.items[i].to, &moved, 0);
    free(moved.items);

    move_preferences(legacy, report.config, &report);

    /* Regenerable, and holding the old paths besides: an inventory that names
       every toolchain where it used to be is worse than none. */
    char disposable[PICKUP_PATHS_MAX];
    if (fs_format_path(disposable, sizeof disposable, "%s/%s", legacy, LEGACY_CACHE))
        (void)fs_remove_tree(disposable);
    if (fs_format_path(disposable, sizeof disposable, "%s/%s", legacy, LEGACY_DOWNLOADS))
        (void)fs_remove_tree(disposable);

    remove_if_empty(legacy, LEGACY_TOOLCHAINS);
    remove_if_empty(legacy, LEGACY_TOOLS);
    note_the_rest(legacy, &report);

    if (report.left_count == 0)
        report.legacy_removed = rmdir(legacy) == 0 || !fs_path_exists(legacy);
    if (!report.legacy_removed)
        leave_marker(&report);
    return report;
}

/* "2 toolchains", "1 tool". */
static void count_of(char *out, size_t out_size, size_t count, const char *noun) {
    snprintf(out, out_size, "%zu %s%s", count, noun, count == 1 ? "" : "s");
}

void migrate_announce(const migrate_report *report, FILE *stream) {
    if (!report->attempted)
        return;

    /* What moved, as a list read aloud: "2 toolchains, 1 tool and
       preferences". */
    const char *parts[3] = {0};
    char toolchains[64] = "";
    char tools[64] = "";
    size_t count = 0;
    if (report->toolchains_moved > 0) {
        count_of(toolchains, sizeof toolchains, report->toolchains_moved, "toolchain");
        parts[count++] = toolchains;
    }
    if (report->tools_moved > 0) {
        count_of(tools, sizeof tools, report->tools_moved, "tool");
        parts[count++] = tools;
    }
    if (report->preferences_moved)
        parts[count++] = "preferences";

    if (count == 0) {
        fprintf(stream, "pickup: nothing in %s needed moving; pickup now keeps its files in %s",
                report->legacy, report->data);
    } else {
        fprintf(stream, "pickup: moved ");
        for (size_t i = 0; i < count; i++)
            fprintf(stream, "%s%s", i == 0 ? "" : (i + 1 == count ? " and " : ", "), parts[i]);
        fprintf(stream, " from %s to %s", report->legacy, report->data);
        if (report->preferences_moved)
            fprintf(stream, " (preferences: %s)", report->config);
    }
    fprintf(stream, "%s\n", report->legacy_removed ? "; the old directory is gone" : "");

    if (report->left_count == 0)
        return;
    fprintf(stream, "pickup: could not move ");
    const size_t named =
        report->left_count < MIGRATE_LEFT_MAX ? report->left_count : MIGRATE_LEFT_MAX;
    for (size_t i = 0; i < named; i++)
        fprintf(stream, "%s%s", i == 0 ? "" : ", ", report->left[i]);
    if (report->left_count > named)
        fprintf(stream, " and %zu more", report->left_count - named);
    fprintf(stream, "; left in %s (see %s there)\n", report->legacy, MIGRATE_MARKER_FILENAME);
}
