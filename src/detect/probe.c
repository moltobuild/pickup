#include <pickup/detect/probe.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/process_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compiler arguments used to interrogate a candidate. */
#define ARG_PREPROCESS "-E"
#define ARG_LANG "-x"
#define LANG_C_NAME "c"
#define ARG_STDIN "-"
#define ARG_DUMPMACHINE "-dumpmachine"
#define ARG_SYNTAX_ONLY "-fsyntax-only"

/* Size of the buffer receiving a compiler's answer. */
#define ANSWER_SIZE 4096

/* Marker the identity program emits, so the answer can be found among the
   preprocessor's other output (line markers, blank lines). */
#define IDENTITY_MARKER "pickup_identity"

/*
 * The identity program. The preprocessor expands it to one line naming the
 * vendor and version, which is the compiler describing itself. Clang also
 * defines __GNUC__, so it has to be tested first.
 */
static const char identity_program[] =
    "#if defined(__apple_build_version__)\n" IDENTITY_MARKER
    " apple-clang __clang_major__ __clang_minor__ __clang_patchlevel__\n"
    "#elif defined(__clang__)\n" IDENTITY_MARKER
    " clang __clang_major__ __clang_minor__ __clang_patchlevel__\n"
    "#elif defined(_MSC_VER)\n" IDENTITY_MARKER " msvc _MSC_VER 0 0\n"
    "#elif defined(__GNUC__)\n" IDENTITY_MARKER " gcc __GNUC__ __GNUC_MINOR__ __GNUC_PATCHLEVEL__\n"
    "#else\n" IDENTITY_MARKER " unknown 0 0 0\n"
    "#endif\n";

/* Find the marker line in the preprocessor's output and read the fields. */
static bool parse_identity(const char *answer, toolchain *out) {
    const char *line = strstr(answer, IDENTITY_MARKER);
    if (line == NULL)
        return false;
    line += strlen(IDENTITY_MARKER);

    char vendor[PICKUP_NAME_MAX];
    int major = 0, minor = 0, patch = 0;
    if (sscanf(line, "%63s %d %d %d", vendor, &major, &minor, &patch) != 4)
        return false;

    out->vendor = toolchain_vendor_parse(vendor);
    out->version = (toolchain_version){.major = major, .minor = minor, .patch = patch};
    return true;
}

/* Strip the trailing newline a compiler leaves on a one-line answer. */
static void trim_newline(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'))
        text[--length] = '\0';
}

bool probe_identify(const char *path, toolchain *out) {
    memset(out, 0, sizeof *out);
    if (!fs_format_path(out->path, sizeof out->path, "%s", path))
        return false;

    /* The name it is known by, not the file it is stored in: everything that
       ranks or transforms a driver reads this. */
    if (!fs_program_name(path, out->name, sizeof out->name))
        return false;

    char answer[ANSWER_SIZE];
    const char *identity_argv[] = {path, ARG_PREPROCESS, ARG_LANG, LANG_C_NAME, ARG_STDIN, NULL};
    process_result result = process_capture(identity_argv, identity_program, answer, sizeof answer);
    if (!result.completed || result.exit_code != 0)
        return false;
    if (!parse_identity(answer, out))
        return false;

    /* The target triple is optional: a compiler without -dumpmachine is still
       usable, it just reports an unknown target. */
    const char *target_argv[] = {path, ARG_DUMPMACHINE, NULL};
    char target[PICKUP_TARGET_MAX];
    result = process_capture(target_argv, NULL, target, sizeof target);
    if (result.completed && result.exit_code == 0) {
        trim_newline(target);
        (void)fs_format_path(out->target, sizeof out->target, "%s", target);
    }
    return true;
}

/* The C driver spellings, and the C++ driver each one leads to. `c++` is
   deliberately absent: it is already the C++ side and has no C counterpart, so
   a toolchain identified through it has no C++ driver to find. */
static const struct {
    const char *c;
    const char *cxx;
} driver_spellings[] = {
    {"gcc", "g++"},
    {"clang", "clang++"},
    {"cc", "c++"},
};
#define DRIVER_SPELLING_COUNT (sizeof driver_spellings / sizeof driver_spellings[0])

/*
 * Where `spelling` sits inside `name` as a whole component: at the front, or
 * directly after a dash.
 *
 * A cross toolchain puts the target triple in front of the driver and nothing
 * else identifies it -- llvm-mingw ships `x86_64-w64-mingw32-gcc`, conda ships
 * `x86_64-conda-linux-gnu-gcc`, and install_service already says so where it
 * looks for one. Anchoring the match at the front of the name finds neither.
 *
 * Requiring a whole component is what makes the search safe: it is the rule
 * that stops `cc` from matching the tail of `gcc`.
 */
static const char *spelling_in(const char *name, const char *spelling) {
    const size_t length = strlen(spelling);
    for (const char *at = name; (at = strstr(at, spelling)) != NULL; at += length) {
        if (at == name || at[-1] == '-')
            return at;
    }
    return NULL;
}

/* The C++ driver name that goes with a C driver, following each vendor's
   convention and carrying any target prefix across untouched:
   gcc-12 -> g++-12, clang-14 -> clang++-14,
   x86_64-w64-mingw32-gcc -> x86_64-w64-mingw32-g++. */
static bool cxx_name_for(const char *c_name, char *out, size_t out_size) {
    for (size_t i = 0; i < DRIVER_SPELLING_COUNT; i++) {
        const char *at = spelling_in(c_name, driver_spellings[i].c);
        if (at == NULL)
            continue;
        return fs_format_path(out, out_size, "%.*s%s%s", (int)(at - c_name), c_name,
                              driver_spellings[i].cxx, at + strlen(driver_spellings[i].c));
    }
    return false;
}

/* True if `path` behaves like a C++ compiler when asked to parse C++. */
static bool runs_as_cxx(const char *path) {
    const char *argv[] = {path, ARG_LANG, "c++", ARG_SYNTAX_ONLY, ARG_STDIN, NULL};
    process_result result = process_try(argv, "int main(){return 0;}\n");
    return result.completed && result.exit_code == 0;
}

void probe_find_cxx_driver(toolchain *chain) {
    chain->cxx_path[0] = '\0';

    char cxx_name[PICKUP_NAME_MAX];
    if (!cxx_name_for(chain->name, cxx_name, sizeof cxx_name))
        return;

    /* The name is what `cxx_name_for` transforms; the file is what the
       filesystem is asked about, and on Windows those differ by `.exe`. */
    char cxx_file[PICKUP_NAME_MAX];
    if (!fs_executable_file(cxx_name, cxx_file, sizeof cxx_file))
        return;

    /* Look for the sibling next to the C driver, so a compiler found in one
       directory does not pair with a different installation in another. */
    char candidate[PICKUP_PATH_MAX];
    const char *slash = strrchr(chain->path, '/');
    bool composed = slash != NULL
                        ? fs_format_path(candidate, sizeof candidate, "%.*s/%s",
                                         (int)(slash - chain->path), chain->path, cxx_file)
                        : fs_format_path(candidate, sizeof candidate, "%s", cxx_file);
    if (!composed || !fs_path_exists(candidate))
        return;

    /* Verified, not assumed: the name existing proves nothing. */
    if (runs_as_cxx(candidate))
        (void)fs_format_path(chain->cxx_path, sizeof chain->cxx_path, "%s", candidate);
}

void probe_capabilities(toolchain *chain) {
    chain->c_features = capability_probe(chain->path, lang_c);
    /* C++ features are probed with the C++ driver. Probing them with the C
       driver would report capabilities of a program that cannot link C++. */
    if (chain->cxx_path[0] != '\0')
        chain->cxx_features = capability_probe(chain->cxx_path, lang_cxx);
    else
        chain->cxx_features = (capability_set){0};
}
