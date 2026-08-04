#include <pickup/detect/gcc_install.h>

#include <pickup/model/toolchain.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/process_service.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* What clang -v is made to do: parse an empty C++ translation unit, which is
   the cheapest way to make the driver run its GCC detection and report it. */
#define ARG_VERBOSE     "-v"
#define ARG_LANG        "-x"
#define LANG_CXX_NAME   "c++"
#define ARG_SYNTAX_ONLY "-fsyntax-only"
#define ARG_STDIN       "-"

/* Keeps the driver's own configuration file out of the answer. Understood by
   Clang and by nothing else, which is why it is only ever passed to a driver
   that has already been seen to accept it. */
#define ARG_NO_DEFAULT_CONFIG "--no-default-config"

/* An empty program: nothing has to compile for the driver to say which GCC it
   picked, and the emptier the input the fewer other diagnostics appear. */
#define EMPTY_PROGRAM "\n"

/* Room for the driver's verbose report. It runs to a few kilobytes. */
#define VERBOSE_SIZE 16384

/* The two lines worth reading out of it. */
#define LINE_CANDIDATE "Found candidate GCC installation:"
#define LINE_SELECTED  "Selected GCC installation:"

/* Where libstdc++ headers sit relative to a GCC installation directory.
   /usr/lib/gcc/<triple>/<version> climbs four levels to /usr, and a toolchain
   with its own prefix climbs to that prefix, so one walk covers both. */
#define LIBSTDCXX_RELATIVE "%s/../../../../include/c++/%s/iostream"

/* Copy the rest of a line into `out`, stopping at the newline and trimming the
   spaces the driver puts after the colon. */
static bool read_line_value(const char *line, char *out, size_t out_size) {
    while (*line == ' ' || *line == '\t')
        line++;

    size_t length = 0;
    while (line[length] != '\0' && line[length] != '\n' && line[length] != '\r')
        length++;
    while (length > 0 && (line[length - 1] == ' ' || line[length - 1] == '\t'))
        length--;

    if (length == 0 || length >= out_size)
        return false;
    memcpy(out, line, length);
    out[length] = '\0';
    return true;
}

/* The version a GCC installation directory is named after: the last path
   component, "12" in /usr/lib/gcc/x86_64-linux-gnu/12. */
static const char *version_of(const char *directory) {
    const char *slash = strrchr(directory, '/');
    return slash != NULL ? slash + 1 : directory;
}

bool gcc_install_has_libstdcxx(const char *directory) {
    if (directory == NULL || directory[0] == '\0')
        return false;

    char header[PICKUP_PATHS_MAX];
    if (!fs_format_path(header, sizeof header, LIBSTDCXX_RELATIVE,
                        directory, version_of(directory)))
        return false;
    /* The header itself, not the directory holding it: an empty include
       directory would otherwise count as a working C++ installation. */
    return fs_path_exists(header);
}

/* Record `path` unless it is already in the list. Clang reports the same
   installation once per search prefix, so duplicates are normal.

   The path is canonicalised when it names something real, because the driver
   answers relative to its own binary — /usr/bin/../lib/gcc/... — and the same
   installation reached through two prefixes would otherwise be recorded twice
   and reported to a reader in a form they never typed. A path that resolves to
   nothing is kept as written: it is still what the driver said. */
static bool add_install(gcc_install_list *list, const char *path) {
    char resolved[PATH_MAX];
    const char *recorded = realpath(path, resolved) != NULL ? resolved : path;

    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].path, recorded) == 0)
            return true;
    }
    if (list->count == GCC_INSTALL_MAX)
        return true; /* a machine with more than this is not worth refusing */

    gcc_install *entry = &list->items[list->count];
    if (!fs_format_path(entry->path, sizeof entry->path, "%s", recorded))
        return false;
    entry->has_libstdcxx = gcc_install_has_libstdcxx(recorded);
    list->count++;
    return true;
}

/* Index of `path` in the list, or GCC_INSTALL_NONE. Canonicalised the same way
   the entries were, so the selected installation is found however the driver
   happened to spell it. */
static size_t index_of(const gcc_install_list *list, const char *path) {
    char resolved[PATH_MAX];
    const char *wanted = realpath(path, resolved) != NULL ? resolved : path;

    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].path, wanted) == 0)
            return i;
    }
    return GCC_INSTALL_NONE;
}

bool gcc_install_parse(const char *verbose_output, gcc_install_list *out) {
    *out = (gcc_install_list){ .count = 0, .selected = GCC_INSTALL_NONE };
    if (verbose_output == NULL)
        return false;

    char selected[PICKUP_PATHS_MAX] = "";

    for (const char *line = verbose_output; line != NULL; ) {
        const char *candidate = strstr(line, LINE_CANDIDATE);
        const char *chosen = strstr(line, LINE_SELECTED);

        /* Whichever comes first, so the two are read in the order the driver
           wrote them and neither scan skips past the other. */
        const char *next;
        bool is_selected;
        if (candidate != NULL && (chosen == NULL || candidate < chosen)) {
            next = candidate + sizeof LINE_CANDIDATE - 1;
            is_selected = false;
        } else if (chosen != NULL) {
            next = chosen + sizeof LINE_SELECTED - 1;
            is_selected = true;
        } else {
            break;
        }

        char path[PICKUP_PATHS_MAX];
        if (read_line_value(next, path, sizeof path)) {
            if (!add_install(out, path))
                return false;
            if (is_selected)
                (void)fs_format_path(selected, sizeof selected, "%s", path);
        }
        line = next;
    }

    if (selected[0] != '\0')
        out->selected = index_of(out, selected);
    return true;
}

bool gcc_install_query(const char *clang_path, bool ignore_config,
                       gcc_install_list *out) {
    *out = (gcc_install_list){ .count = 0, .selected = GCC_INSTALL_NONE };
    if (clang_path == NULL || clang_path[0] == '\0')
        return false;

    const char *bare[] = {
        clang_path, ARG_NO_DEFAULT_CONFIG, ARG_VERBOSE, ARG_LANG, LANG_CXX_NAME,
        ARG_SYNTAX_ONLY, ARG_STDIN, NULL
    };
    const char *configured[] = {
        clang_path, ARG_VERBOSE, ARG_LANG, LANG_CXX_NAME,
        ARG_SYNTAX_ONLY, ARG_STDIN, NULL
    };
    const char *const *argv = ignore_config ? bare : configured;
    char answer[VERBOSE_SIZE];
    /* The report goes to stderr; stdout carries nothing here. */
    process_result result = process_capture_stderr(argv, EMPTY_PROGRAM,
                                                   answer, sizeof answer);
    if (!result.completed)
        return false;

    /* A non-zero exit is not a reason to discard the report: the driver prints
       what it found before it gets as far as failing. */
    return gcc_install_parse(answer, out);
}

bool gcc_install_best(const gcc_install_list *list, gcc_install *out) {
    bool found = false;
    toolchain_version best = { 0 };

    for (size_t i = 0; i < list->count; i++) {
        if (!list->items[i].has_libstdcxx)
            continue;

        toolchain_version version;
        if (!toolchain_version_parse(version_of(list->items[i].path), &version))
            continue;
        if (found && toolchain_version_compare(version, best) <= 0)
            continue;

        best = version;
        *out = list->items[i];
        found = true;
    }
    return found;
}
