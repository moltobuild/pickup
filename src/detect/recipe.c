#include <pickup/detect/recipe.h>

#include <pickup/detect/gcc_install.h>
#include <pickup/detect/health.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/process_service.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Asking a driver where one of its own files lives. It answers with a full
   path when it has the file, and echoes the name back when it does not. */
#define ARG_PRINT_FILE_NAME "-print-file-name=%s"
#define LIBCXX_SHARED       "libc++.so"
#define LIBCXX_DYLIB        "libc++.dylib"
#define LIBSTDCXX_SHARED    "libstdc++.so"

/* The flags a candidate may carry.

   Two ways of naming a GCC, because only one of them is old enough to be
   everywhere: --gcc-install-dir names a version exactly and arrived in Clang
   16, while --gcc-toolchain names a prefix and has been understood for far
   longer. Neither is assumed to work — both are candidates, and the driver
   decides by accepting one or refusing it. */
#define FLAG_STDLIB_LIBCXX  "-stdlib=libc++"
#define FLAG_GCC_INSTALL    "--gcc-install-dir=%s"
#define FLAG_GCC_TOOLCHAIN  "--gcc-toolchain=%s"
#define FLAG_RPATH          "-Wl,-rpath,%s"

/* How far above a GCC installation directory its prefix sits:
   <prefix>/lib/gcc/<triple>/<version> is four levels down. */
#define GCC_PREFIX_DEPTH 4

/* Flags starting with this reach the linker only, and putting them on a
   compile-only command line would be an unused argument at best. */
#define LINKER_FLAG_PREFIX "-Wl,"

/* The run-time search path flag, whose value is a directory a caller needs to
   know about in its own right. */
#define RPATH_FLAG_PREFIX "-Wl,-rpath,"

#define ANSWER_SIZE 4096

/* One configuration worth trying, held as borrowed strings so building the
   list costs nothing until one of them wins. */
typedef struct {
    cxx_stdlib stdlib;
    const char *flags[RECIPE_MAX_FLAGS];
    size_t count;
} candidate;

const char *recipe_stdlib_name(cxx_stdlib stdlib) {
    switch (stdlib) {
    case stdlib_libstdcxx: return "libstdc++";
    case stdlib_libcxx:    return "libc++";
    case stdlib_unknown:   return "";
    }
    return "";
}

/* The directory part of `path`, into `out`. */
static bool directory_of(const char *path, char *out, size_t out_size) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path)
        return false;
    return fs_format_path(out, out_size, "%.*s", (int)(slash - path), path);
}

/* Where `compiler` keeps one of its own libraries, asked of the compiler.

   It answers with a full path when it has the file and echoes the name back
   when it does not, so a bare name is the negative answer. */
static bool own_library_dir(const char *compiler, const char *const *names,
                            size_t name_count, char *out, size_t out_size) {
    if (compiler == NULL || compiler[0] == '\0')
        return false;

    for (size_t i = 0; i < name_count; i++) {
        char argument[64];
        snprintf(argument, sizeof argument, ARG_PRINT_FILE_NAME, names[i]);

        const char *argv[] = { compiler, argument, NULL };
        char answer[ANSWER_SIZE];
        process_result result = process_capture(argv, NULL, answer, sizeof answer);
        if (!result.completed || result.exit_code != 0)
            continue;

        char *newline = strchr(answer, '\n');
        if (newline != NULL)
            *newline = '\0';
        if (strchr(answer, '/') == NULL || !fs_path_exists(answer))
            continue;

        /* Canonicalised because the driver answers relative to its own binary,
           as bin/../lib/..., and that path ends up written into an executable
           and read back by the loader. */
        char resolved[PATH_MAX];
        const char *usable = realpath(answer, resolved) != NULL ? resolved : answer;
        return directory_of(usable, out, out_size);
    }
    return false;
}

bool recipe_own_libcxx_dir(const char *compiler, char *out, size_t out_size) {
    static const char *const names[] = { LIBCXX_SHARED, LIBCXX_DYLIB };
    return own_library_dir(compiler, names, sizeof names / sizeof names[0],
                           out, out_size);
}

/* The same for the GNU standard library.

   A GCC that Pickup installed carries its own, newer than the system's, and
   links against it — but the loader finds the system's first, and the program
   dies on a missing symbol version. That is the same failure as a Clang
   linking its own libc++, and it takes the same answer: name the directory the
   library is actually in. */
static bool own_libstdcxx_dir(const char *compiler, char *out, size_t out_size) {
    static const char *const names[] = { LIBSTDCXX_SHARED };
    return own_library_dir(compiler, names, sizeof names / sizeof names[0],
                           out, out_size);
}

/* Add a flag to a candidate, ignoring it once the candidate is full. */
static void add_flag(candidate *entry, const char *flag) {
    if (entry->count < RECIPE_MAX_FLAGS)
        entry->flags[entry->count++] = flag;
}

/*
 * Programs that compile only against the library they name.
 *
 * Every standard library defines its own version macro in every header it
 * ships, so including the smallest one is enough to tell them apart. <cstddef>
 * is that header, and it exists in every version of both.
 *
 * The answer is the exit status rather than anything printed, which is what
 * makes it reliable: preprocessing a standard header produces tens of
 * kilobytes, and a marker emitted after the include would sit past the end of
 * any fixed buffer that read it back. Asking a question whose answer is one
 * bit avoids the whole problem, and is how capability_probe works already.
 */
static const char program_is_libcxx[] =
    "#include <cstddef>\n"
    "#if !defined(_LIBCPP_VERSION)\n"
    "#error not libc++\n"
    "#endif\n"
    "int main(){return 0;}\n";

static const char program_is_libstdcxx[] =
    "#include <cstddef>\n"
    "#if !defined(__GLIBCXX__) && !defined(__GLIBCPP__)\n"
    "#error not libstdc++\n"
    "#endif\n"
    "int main(){return 0;}\n";

/* True if `program` compiles with these flags. */
static bool accepts_program(const char *compiler, const char *const *flags,
                            size_t flag_count, const char *program) {
    const char *argv[RECIPE_MAX_FLAGS + 8];
    size_t limit = sizeof argv / sizeof argv[0];
    size_t count = 0;

    argv[count++] = compiler;
    argv[count++] = "-x";
    argv[count++] = "c++";
    for (size_t i = 0; i < flag_count && count + 3 < limit; i++)
        argv[count++] = flags[i];
    argv[count++] = "-fsyntax-only";
    argv[count++] = "-";
    argv[count] = NULL;

    process_result result = process_try(argv, program);
    return result.completed && result.exit_code == 0;
}

cxx_stdlib recipe_detect_stdlib(const char *compiler, const char *const *flags,
                                size_t flag_count) {
    if (compiler == NULL || compiler[0] == '\0')
        return stdlib_unknown;

    if (accepts_program(compiler, flags, flag_count, program_is_libcxx))
        return stdlib_libcxx;
    if (accepts_program(compiler, flags, flag_count, program_is_libstdcxx))
        return stdlib_libstdcxx;
    return stdlib_unknown;
}

/*
 * Build the candidates for `chain`, in the order they should be tried.
 *
 * `storage` holds the composed flags, which have to outlive this call because
 * the candidates borrow them.
 */
typedef struct {
    char gcc_install[RECIPE_FLAG_MAX];
    char gcc_toolchain[RECIPE_FLAG_MAX];
    char rpath[RECIPE_FLAG_MAX];
    char libcxx_dir[PICKUP_PATHS_MAX];
    bool has_libcxx;
    char own_rpath[RECIPE_FLAG_MAX];
    char own_stdlib_dir[PICKUP_PATHS_MAX];
    bool has_own_stdlib;
} candidate_storage;

/* Climb `levels` path components. /usr/lib/gcc/<triple>/11 becomes /usr, and
   a toolchain installed under its own prefix becomes that prefix. */
static bool climb(const char *path, int levels, char *out, size_t out_size) {
    if (!fs_format_path(out, out_size, "%s", path))
        return false;
    for (int i = 0; i < levels; i++) {
        char *slash = strrchr(out, '/');
        if (slash == NULL || slash == out)
            return false;
        *slash = '\0';
    }
    return out[0] != '\0';
}

static size_t build_candidates(const toolchain *chain, capability_lang lang,
                               const char *driver, candidate_storage *storage,
                               candidate *out, size_t max) {
    size_t count = 0;

    /* 1. Nothing added. A toolchain that works untouched is the one to
          publish: no flags is the most portable answer there is. */
    if (count < max)
        out[count++] = (candidate){ .stdlib = stdlib_unknown, .count = 0 };

    /* 2. Name where the toolchain's own C++ library lives.

          A compiler Pickup installed brings a newer libstdc++ than the system
          has and links against it, while the loader goes on finding the
          system's first — so the program builds and then dies on a missing
          symbol version. Nothing about that shows up until it is run, which is
          why running it is part of the test. */
    storage->has_own_stdlib = lang == lang_cxx
        && own_libstdcxx_dir(driver, storage->own_stdlib_dir,
                             sizeof storage->own_stdlib_dir);
    if (storage->has_own_stdlib
        && fs_format_path(storage->own_rpath, sizeof storage->own_rpath,
                          FLAG_RPATH, storage->own_stdlib_dir)
        && count < max) {
        candidate entry = { .stdlib = stdlib_libstdcxx, .count = 0 };
        add_flag(&entry, storage->own_rpath);
        out[count++] = entry;
    }

    /* 3. Pin the GCC installation, when a complete one exists and is not
          already what the driver would choose. This is what answers a Clang
          that picked the highest-numbered GCC and found no C++ in it. */
    gcc_install_list installs;
    gcc_install best;
    bool have_best = gcc_install_query(driver, &installs) && installs.count > 0
        && gcc_install_best(&installs, &best);

    if (have_best
        && fs_format_path(storage->gcc_install, sizeof storage->gcc_install,
                          FLAG_GCC_INSTALL, best.path)
        && count < max) {
        candidate entry = { .stdlib = stdlib_libstdcxx, .count = 0 };
        add_flag(&entry, storage->gcc_install);
        out[count++] = entry;
    }

    /* 4. The same intent through the older flag, for drivers that do not know
          the newer one. It names a prefix rather than a version, so it only
          helps where the wanted GCC is the one that prefix leads to — which is
          exactly the case for a GCC Pickup installed itself. */
    char prefix[PICKUP_PATHS_MAX];
    if (have_best && climb(best.path, GCC_PREFIX_DEPTH, prefix, sizeof prefix)
        && fs_format_path(storage->gcc_toolchain, sizeof storage->gcc_toolchain,
                          FLAG_GCC_TOOLCHAIN, prefix)
        && count < max) {
        candidate entry = { .stdlib = stdlib_libstdcxx, .count = 0 };
        add_flag(&entry, storage->gcc_toolchain);
        out[count++] = entry;
    }

    /* 5. The compiler's own libc++, with the run-time search path it needs.
          Only for C++, and only when the toolchain actually carries one.

          The rpath is not optional. Without it the link succeeds and the
          program dies on its first run, which is precisely the failure this
          module was built to stop reporting as success. */
    storage->has_libcxx = lang == lang_cxx
        && recipe_own_libcxx_dir(driver, storage->libcxx_dir,
                                 sizeof storage->libcxx_dir);
    if (storage->has_libcxx && count < max) {
        candidate entry = { .stdlib = stdlib_libcxx, .count = 0 };
        add_flag(&entry, FLAG_STDLIB_LIBCXX);
        if (fs_format_path(storage->rpath, sizeof storage->rpath,
                           FLAG_RPATH, storage->libcxx_dir))
            add_flag(&entry, storage->rpath);
        /* Pinning the GCC as well, when there is one to pin: libc++ still
           borrows the startup objects and libgcc from it. */
        if (storage->gcc_install[0] != '\0')
            add_flag(&entry, storage->gcc_install);
        out[count++] = entry;
    }

    (void)chain;
    return count;
}

/* Record a winning candidate as the published recipe, splitting its flags
   between the two command lines they belong on. */
static void publish(const candidate *winner, const candidate_storage *storage,
                    cxx_stdlib stdlib, link_recipe *recipe) {
    recipe->usable = true;
    recipe->stdlib = stdlib;

    for (size_t i = 0; i < winner->count; i++) {
        const char *flag = winner->flags[i];
        bool linker_only = strncmp(flag, LINKER_FLAG_PREFIX,
                                   sizeof LINKER_FLAG_PREFIX - 1) == 0;

        if (!linker_only && recipe->compile_count < RECIPE_MAX_FLAGS)
            (void)fs_format_path(recipe->compile_flags[recipe->compile_count++],
                                 RECIPE_FLAG_MAX, "%s", flag);
        if (recipe->link_count < RECIPE_MAX_FLAGS)
            (void)fs_format_path(recipe->link_flags[recipe->link_count++],
                                 RECIPE_FLAG_MAX, "%s", flag);

        /* Reported only when the recipe had to name a search path to get the
           program running, and taken from the flag itself rather than worked
           out again: a library the loader already finds is not something a
           caller has to carry around. */
        if (strncmp(flag, RPATH_FLAG_PREFIX, sizeof RPATH_FLAG_PREFIX - 1) == 0
            && recipe->runtime_count < RECIPE_MAX_DIRS)
            (void)fs_format_path(recipe->runtime_dirs[recipe->runtime_count++],
                                 PICKUP_PATHS_MAX, "%s",
                                 flag + sizeof RPATH_FLAG_PREFIX - 1);
    }
    (void)storage;
}

/* The two ways a recipe may name a GCC installation. */
#define GCC_FLAG_INSTALL_PREFIX   "--gcc-install-dir="
#define GCC_FLAG_TOOLCHAIN_PREFIX "--gcc-toolchain="

static bool is_gcc_flag(const char *flag) {
    return strncmp(flag, GCC_FLAG_INSTALL_PREFIX,
                   sizeof GCC_FLAG_INSTALL_PREFIX - 1) == 0
        || strncmp(flag, GCC_FLAG_TOOLCHAIN_PREFIX,
                   sizeof GCC_FLAG_TOOLCHAIN_PREFIX - 1) == 0;
}

const char *recipe_gcc_flag(const link_recipe *recipe) {
    for (size_t i = 0; i < recipe->compile_count; i++) {
        if (is_gcc_flag(recipe->compile_flags[i]))
            return recipe->compile_flags[i];
    }
    return NULL;
}

bool recipe_align_gcc(const link_recipe *cxx, const char *driver, link_recipe *c) {
    const char *flag = recipe_gcc_flag(cxx);
    if (flag == NULL || !c->usable || recipe_gcc_flag(c) != NULL)
        return false;
    if (c->compile_count >= RECIPE_MAX_FLAGS || c->link_count >= RECIPE_MAX_FLAGS)
        return false;

    /* Checked before it is kept: C already worked without this, so a flag that
       broke it would trade a mismatch for a compiler that builds nothing. */
    const char *candidate[RECIPE_MAX_FLAGS + 1];
    size_t count = 0;
    for (size_t i = 0; i < c->compile_count && count < RECIPE_MAX_FLAGS; i++)
        candidate[count++] = c->compile_flags[i];
    candidate[count++] = flag;

    if (health_probe(driver, lang_c, candidate, count) != health_ok)
        return false;

    (void)fs_format_path(c->compile_flags[c->compile_count++], RECIPE_FLAG_MAX,
                         "%s", flag);
    (void)fs_format_path(c->link_flags[c->link_count++], RECIPE_FLAG_MAX,
                         "%s", flag);
    return true;
}

/* What a driver's own configuration file is called: the driver's name with
   this appended, in the directory the driver sits in. */
#define CONFIG_SUFFIX ".cfg"

bool recipe_write_config(const char *driver, const link_recipe *recipe) {
    if (driver == NULL || driver[0] == '\0' || !recipe->usable)
        return false;
    /* A recipe with no flags is the compiler working as it is. Writing an
       empty file would only leave something to wonder about later. */
    if (recipe->link_count == 0)
        return false;

    char path[PICKUP_PATHS_MAX];
    if (!fs_format_path(path, sizeof path, "%s" CONFIG_SUFFIX, driver))
        return false;

    /* One flag per line, which is the format these files take. The link flags
       are the superset: everything needed to compile is needed to link too,
       and a flag that only matters to the linker is ignored when compiling. */
    char contents[RECIPE_MAX_FLAGS * RECIPE_FLAG_MAX + 256];
    int written = snprintf(contents, sizeof contents,
                           "# Written by pickup: the configuration this "
                           "toolchain was proven to build with.\n");
    if (written < 0)
        return false;

    size_t used = (size_t)written;
    for (size_t i = 0; i < recipe->link_count && used < sizeof contents; i++) {
        int line = snprintf(contents + used, sizeof contents - used, "%s\n",
                            recipe->link_flags[i]);
        if (line < 0)
            return false;
        used += (size_t)line;
    }
    return fs_write_file(path, contents);
}

link_recipe recipe_discover(const toolchain *chain, capability_lang lang) {
    return recipe_discover_for(chain, lang, stdlib_unknown);
}

link_recipe recipe_discover_for(const toolchain *chain, capability_lang lang,
                                cxx_stdlib wanted) {
    link_recipe recipe = { 0 };

    const char *driver = lang == lang_c ? chain->path : chain->cxx_path;
    if (driver == NULL || driver[0] == '\0')
        return recipe;

    candidate_storage storage = { 0 };
    candidate candidates[RECIPE_MAX_FLAGS];
    size_t count = build_candidates(chain, lang, driver, &storage,
                                    candidates, RECIPE_MAX_FLAGS);

    for (size_t i = 0; i < count; i++) {
        health_status status = health_probe(driver, lang, candidates[i].flags,
                                            candidates[i].count);
        if (status != health_ok)
            continue;

        /* Which library it ended up using is read back from the compiler, not
           taken from what the candidate intended. The first candidate names no
           library at all and still uses one, and which one that is depends on
           the platform. */
        cxx_stdlib stdlib = lang == lang_cxx
            ? recipe_detect_stdlib(driver, candidates[i].flags, candidates[i].count)
            : stdlib_unknown;

        /* A caller that named a library is stating an ABI constraint, so a
           working recipe using the other one is not an acceptable answer:
           keep looking rather than settling. */
        if (wanted != stdlib_unknown && stdlib != wanted)
            continue;

        publish(&candidates[i], &storage, stdlib, &recipe);
        return recipe;
    }
    return recipe;
}
