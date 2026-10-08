#include <pickup/detect/tools.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/process_service.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What every one of these answers to. */
#define ARG_VERSION "--version"

/* Room for the answer, which is a line or two. */
#define ANSWER_SIZE 512

/*
 * What Pickup looks for, in order of preference within each kind.
 *
 * clang-format and clang-tidy come first because a toolchain of LLVM already
 * carries them, so a machine with one has them for free. cppcheck is a linter
 * that predates clang-tidy and is still what many projects use.
 */
typedef struct {
    const char *name;
    tool_kind kind;
} tool_candidate;

static const tool_candidate candidates[] = {
    {"clang-format", tool_formatter},
    {"clang-tidy", tool_linter},
    {"cppcheck", tool_linter},
    {"clangd", tool_language_server},
    {"cmake", tool_build},
    {"ninja", tool_build},
    /* What x86 assembly in a dependency is assembled with (molto RFC-0025). */
    {"nasm", tool_build},
};

#define CANDIDATE_COUNT (sizeof candidates / sizeof candidates[0])

/* What `install` is told to fetch for each kind. */
#define PACKAGE_FORMATTER "clang-format"
#define PACKAGE_LINTER "clang-tidy"
#define PACKAGE_LANGUAGE_SERVER "clangd"
#define PACKAGE_BUILD "cmake"

const char *tool_kind_name(tool_kind kind) {
    switch (kind) {
    case tool_formatter:
        return "formatter";
    case tool_linter:
        return "linter";
    case tool_language_server:
        return "language server";
    case tool_build:
        return "build";
    }
    return "tool";
}

const char *tool_kind_package(tool_kind kind) {
    switch (kind) {
    case tool_formatter:
        return PACKAGE_FORMATTER;
    case tool_linter:
        return PACKAGE_LINTER;
    case tool_language_server:
        return PACKAGE_LANGUAGE_SERVER;
    case tool_build:
        return PACKAGE_BUILD;
    }
    return PACKAGE_FORMATTER;
}

bool tools_kind_of(const char *name, tool_kind *kind) {
    if (name == NULL)
        return false;
    for (size_t i = 0; i < CANDIDATE_COUNT; i++) {
        if (strcmp(candidates[i].name, name) == 0) {
            *kind = candidates[i].kind;
            return true;
        }
    }
    return false;
}

/*
 * Pull the version out of what a tool printed.
 *
 * Not simply the first line. clang-format answers with its version followed by
 * the URL it was built from and a commit hash, which is provenance rather than
 * identity; clang-tidy opens with a banner and puts the version on the second
 * line:
 *
 *     LLVM (http://llvm.org/):
 *       LLVM version 22.1.8
 *
 * So the line carrying the word is the one taken, and the parenthesis that
 * usually follows is dropped. A tool that words it differently — cppcheck says
 * "Cppcheck 2.21.0" — falls back to its first line, which is right for it.
 */
static void extract_version(char *text) {
    char *chosen = NULL;

    for (char *line = text; line != NULL && *line != '\0';) {
        char *end = strpbrk(line, "\r\n");
        if (end != NULL)
            *end = '\0';

        while (*line == ' ' || *line == '\t')
            line++;
        if (*line != '\0' && chosen == NULL)
            chosen = line; /* the first line, as a fallback */
        if (strstr(line, "version") != NULL) {
            chosen = line;
            break;
        }
        line = end != NULL ? end + 1 : NULL;
    }

    if (chosen == NULL) {
        text[0] = '\0';
        return;
    }

    char *paren = strstr(chosen, " (");
    if (paren != NULL)
        *paren = '\0';
    memmove(text, chosen, strlen(chosen) + 1);
}

/* Ask the binary at `path` to identify itself. False when it does not answer,
   which is the difference between a tool and a file with the right name. */
static bool interrogate(const char *path, char *version, size_t version_size) {
    const char *argv[] = {path, ARG_VERSION, NULL};
    char answer[ANSWER_SIZE];
    process_result result = process_capture(argv, NULL, answer, sizeof answer);
    if (!result.completed || result.exit_code != 0)
        return false;

    extract_version(answer);
    if (answer[0] == '\0')
        return false;
    /* Truncated rather than refused if it runs long: this is a line of text to
       show a reader, not a path where a missing tail would mean a different
       file. */
    (void)snprintf(version, version_size, "%.*s", (int)(version_size - 1), answer);
    return true;
}

/* Record `path` as `candidate`, if it runs. */
static bool accept(const tool_candidate *candidate, const char *path, toolchain_source source,
                   dev_tool *out) {
    char version[TOOL_VERSION_MAX];
    if (!interrogate(path, version, sizeof version))
        return false;

    *out = (dev_tool){.kind = candidate->kind, .source = source};
    (void)fs_format_path(out->name, sizeof out->name, "%s", candidate->name);
    (void)fs_format_path(out->path, sizeof out->path, "%s", path);
    (void)fs_format_path(out->version, sizeof out->version, "%s", version);
    return true;
}

/* Look for `candidate` inside one directory. */
static bool find_in(const char *directory, const tool_candidate *candidate, toolchain_source source,
                    dev_tool *out) {
    /* The name is what the tool is called; the file is what the directory
       holds, and on Windows those differ by `.exe`. */
    char file[PICKUP_NAME_MAX];
    if (!fs_executable_file(candidate->name, file, sizeof file))
        return false;

    char path[PICKUP_PATHS_MAX];
    if (!fs_format_path(path, sizeof path, "%s/%s", directory, file))
        return false;
    if (!fs_path_exists(path))
        return false;
    return accept(candidate, path, source, out);
}

/* What one directory of PATH is being asked, and what it answered. */
typedef struct {
    const tool_candidate *candidate;
    dev_tool *out;
    bool found;
} tool_search;

static bool visit_for_tool(const char *directory, void *context) {
    tool_search *search = context;
    search->found = find_in(directory, search->candidate, toolchain_source_system, search->out);
    return !search->found; /* the first answer ends the walk */
}

/* Walk PATH looking for one candidate. */
static bool find_on_path(const tool_candidate *candidate, dev_tool *out) {
    tool_search search = {.candidate = candidate, .out = out, .found = false};
    (void)fs_walk_path(getenv("PATH"), visit_for_tool, &search);
    return search.found;
}

/* The version a tool reported, read from the first number in its answer that
   has a dot after it: "clang-format version 21.1.8 (...)" and "LLVM version
   21.1.8" both lead with text, and a bare "21" is not a version. */
static bool reported_version(const char *answer, toolchain_version *out) {
    for (const char *at = answer; *at != '\0'; at++) {
        if (*at < '0' || *at > '9')
            continue;
        const char *digits = at;
        while (*at >= '0' && *at <= '9')
            at++;
        if (*at == '.' && toolchain_version_parse(digits, out))
            return true;
        if (*at == '\0')
            break;
    }
    return false;
}

/* Whether `candidate` reported a newer version than `best`. One that reported
   no readable version never displaces one that did. */
static bool is_newer(const dev_tool *candidate, const dev_tool *best) {
    toolchain_version mine;
    toolchain_version theirs;
    if (!reported_version(candidate->version, &mine))
        return false;
    if (!reported_version(best->version, &theirs))
        return true;
    return toolchain_version_compare(mine, theirs) > 0;
}

/*
 * And in the bin directory of everything Pickup installed, which is not on
 * PATH and would otherwise be invisible — a clang toolchain carries both of
 * these, so missing them would report a machine as barer than it is.
 *
 * Every directory is asked, and the newest answer wins. Several versions of
 * one tool side by side is what an upgrade leaves behind, and the order
 * `readdir` lists them in is the filesystem's, not anyone's choice: stopping
 * at the first one answered with whichever the disk happened to store first,
 * which on a Mac was the older, broken one.
 */
static bool find_under(const char *root, const tool_candidate *candidate, dev_tool *out) {
    DIR *dir = opendir(root);
    if (dir == NULL)
        return false;

    bool found = false;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        /* In bin/, as a toolchain and CMake keep it, or at the top, as an
           archive holding nothing else does: Ninja's (RFC-0023). */
        char bin[PICKUP_PATHS_MAX];
        char top[PICKUP_PATHS_MAX];
        if (!fs_format_path(bin, sizeof bin, "%s/%s/bin", root, entry->d_name) ||
            !fs_format_path(top, sizeof top, "%s/%s", root, entry->d_name))
            continue;
        dev_tool answer;
        if (!find_in(bin, candidate, toolchain_source_pickup, &answer) &&
            !find_in(top, candidate, toolchain_source_pickup, &answer))
            continue;
        if (!found || is_newer(&answer, out)) {
            *out = answer;
            found = true;
        }
    }
    closedir(dir);
    return found;
}

/* Both places Pickup installs into: toolchains carry a formatter and a linter
   of their own, and tools are installed on their own. */
static bool find_in_pickup(const tool_candidate *candidate, dev_tool *out) {
    char directory[PICKUP_PATHS_MAX];
    if (paths_tools(directory, sizeof directory) && find_under(directory, candidate, out))
        return true;
    return paths_toolchains(directory, sizeof directory) && find_under(directory, candidate, out);
}

size_t tools_discover(dev_tool *out, size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < CANDIDATE_COUNT && count < max; i++) {
        if (find_on_path(&candidates[i], &out[count]) ||
            find_in_pickup(&candidates[i], &out[count]))
            count++;
    }
    return count;
}
