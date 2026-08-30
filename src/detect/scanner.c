#include <pickup/detect/scanner.h>

#include <pickup/services/fs_service.h>

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Size of the buffer used to compose the path of a directory entry. */
#define SCANNER_PATH_SIZE 4096
/* A directory entry's name, which no filesystem lets past 255. */
#define SCANNER_NAME_SIZE 256

/* Names that make a file worth probing. A prefix match also accepts the
   versioned forms the distributions ship (gcc-12, clang-14). */
static const char *const candidate_prefixes[] = {
    "gcc", "g++", "clang", "clang++", "tcc",
};

/* Names that are candidates only when they match exactly: `cc` and `c++` are
   the conventional entry points, but accepting them as prefixes would drag in
   every unrelated tool starting with those letters. */
static const char *const candidate_exact[] = {
    "cc",
    "c++",
};

#define PREFIX_COUNT (sizeof candidate_prefixes / sizeof candidate_prefixes[0])
#define EXACT_COUNT (sizeof candidate_exact / sizeof candidate_exact[0])

/* True if `text` is a version suffix: empty, or "-" followed by a digit.
   Distinguishes arm-none-eabi-gcc-12 from arm-none-eabi-gcc-wrapper. */
static bool is_version_suffix(const char *text) {
    if (text[0] == '\0')
        return true;
    return text[0] == '-' && text[1] >= '0' && text[1] <= '9';
}

/*
 * True if `name` is the prefix followed by nothing or by a version.
 *
 * What follows has to be a version and not merely a dash. A GCC installation
 * puts `gcc-ar`, `gcc-nm` and `gcc-ranlib` beside the compiler, once per
 * version, and those are binutils wrappers with no compiler in them: on an
 * ordinary machine they are nine of the twenty-four candidates, each one
 * interrogated on every scan only to be found not to be a compiler at all.
 */
static bool matches_prefix(const char *name, const char *prefix) {
    size_t length = strlen(prefix);
    if (strncmp(name, prefix, length) != 0)
        return false;
    return is_version_suffix(name + length);
}

/* True if `name` carries `tool` as a trailing component, with or without a
   version: arm-none-eabi-gcc and arm-none-eabi-gcc-12 both name a compiler. */
static bool has_tool_suffix(const char *name, const char *tool) {
    size_t tool_length = strlen(tool);
    for (const char *cursor = name; (cursor = strchr(cursor, '-')) != NULL; cursor++) {
        if (strncmp(cursor + 1, tool, tool_length) != 0)
            continue;
        if (is_version_suffix(cursor + 1 + tool_length))
            return true;
    }
    return false;
}

bool scanner_is_candidate_name(const char *name) {
    for (size_t i = 0; i < EXACT_COUNT; i++) {
        if (strcmp(name, candidate_exact[i]) == 0)
            return true;
    }
    for (size_t i = 0; i < PREFIX_COUNT; i++) {
        if (matches_prefix(name, candidate_prefixes[i]))
            return true;
    }
    /* Cross compilers carry a target triple in front: arm-none-eabi-gcc. */
    for (size_t i = 0; i < PREFIX_COUNT; i++) {
        if (has_tool_suffix(name, candidate_prefixes[i]))
            return true;
    }
    return false;
}

/* Candidates as found, plus what each one resolves to. Two lists rather than
   one: the resolved path decides identity, but the path as found is what gets
   reported, because that is the name a user types and a build invokes. */
typedef struct {
    str_list found;
    str_list resolved;
} candidate_list;

/* The part of a path after the last separator. */
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* Record `path` unless the binary it resolves to is already recorded. When it
   is, keep whichever spelling is shorter: `gcc-12` reads better than
   `x86_64-linux-gnu-gcc-12`, and both invoke the same compiler. */
static bool add_unique(candidate_list *list, const char *path) {
    char resolved[PATH_MAX];
    if (!fs_real_path(path, resolved, sizeof resolved))
        return true; /* a broken link is not an error, just not a compiler */

    for (size_t i = 0; i < str_list_count(&list->resolved); i++) {
        if (strcmp(str_list_get(&list->resolved, i), resolved) != 0)
            continue;
        const char *kept = str_list_get(&list->found, i);
        if (strlen(basename_of(path)) < strlen(basename_of(kept)))
            return str_list_replace(&list->found, i, path);
        return true;
    }
    return str_list_push(&list->found, path) && str_list_push(&list->resolved, resolved);
}

/* Scan one directory of PATH for candidates. */
static bool scan_directory(const char *directory, candidate_list *out) {
    DIR *dir = opendir(directory);
    if (dir == NULL)
        return true; /* unreadable or missing entries in PATH are normal */

    bool ok = true;
    const struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        /* The name without the platform's executable suffix, and a refusal
           when the file cannot be run by name at all — which on Windows is the
           filter `access(X_OK)` below cannot be, since there is no execute bit
           there for it to read. */
        char bare[SCANNER_NAME_SIZE];
        if (!fs_executable_name(entry->d_name, bare, sizeof bare))
            continue;
        if (!scanner_is_candidate_name(bare))
            continue;
        char path[SCANNER_PATH_SIZE];
        if (!fs_format_path(path, sizeof path, "%s/%s", directory, entry->d_name))
            continue;
        if (access(path, X_OK) != 0)
            continue;
        ok = add_unique(out, path);
    }
    closedir(dir);
    return ok;
}

/* Copy the candidate list back out, replacing what the caller had. Rebuilt
   rather than appended to, because deduplication may have swapped an existing
   entry for a better-named spelling of the same binary. */
static bool publish(const candidate_list *candidates, str_list *out) {
    str_list_free(out);
    str_list_init(out);
    for (size_t i = 0; i < str_list_count(&candidates->found); i++) {
        if (!str_list_push(out, str_list_get(&candidates->found, i)))
            return false;
    }
    return true;
}

/* Start from what has already been collected, so anything added afterwards is
   deduplicated against it. */
static bool seed(const str_list *existing, candidate_list *candidates) {
    for (size_t i = 0; i < str_list_count(existing); i++) {
        if (!add_unique(candidates, str_list_get(existing, i)))
            return false;
    }
    return true;
}

bool scanner_collect_installed(const char *toolchains_dir, str_list *out) {
    DIR *dir = opendir(toolchains_dir);
    if (dir == NULL)
        return true; /* nothing installed yet */

    candidate_list candidates;
    str_list_init(&candidates.found);
    str_list_init(&candidates.resolved);

    bool ok = seed(out, &candidates);
    const struct dirent *entry;
    while (ok && (entry = readdir(dir)) != NULL) {
        /* Skips . and .., and also .partial, which is an install in flight. */
        if (entry->d_name[0] == '.')
            continue;

        char bin[SCANNER_PATH_SIZE];
        if (!fs_format_path(bin, sizeof bin, "%s/%s/bin", toolchains_dir, entry->d_name))
            continue;
        if (!fs_is_dir(bin))
            continue;
        ok = scan_directory(bin, &candidates);
    }
    closedir(dir);

    if (ok)
        ok = publish(&candidates, out);
    str_list_free(&candidates.found);
    str_list_free(&candidates.resolved);
    return ok;
}

/* One directory of PATH, for the walk below. A directory that cannot be read
   stops nothing; one that runs out of memory stops everything. */
static bool visit_for_candidates(const char *directory, void *context) {
    return scan_directory(directory, context);
}

bool scanner_collect(const char *path_env, str_list *out) {
    if (path_env == NULL)
        return true;

    candidate_list candidates;
    str_list_init(&candidates.found);
    str_list_init(&candidates.resolved);

    bool ok = fs_walk_path(path_env, visit_for_candidates, &candidates);
    for (size_t i = 0; ok && i < str_list_count(&candidates.found); i++)
        ok = str_list_push(out, str_list_get(&candidates.found, i));

    str_list_free(&candidates.found);
    str_list_free(&candidates.resolved);
    return ok;
}
