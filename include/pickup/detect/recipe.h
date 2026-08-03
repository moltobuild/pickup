#ifndef PICKUP_RECIPE_H
#define PICKUP_RECIPE_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/detect/capability.h>
#include <pickup/model/toolchain.h>
#include <pickup/services/paths_service.h>

/*
 * How to build with a toolchain, as opposed to which toolchain to build with.
 *
 * A compiler is not a command; it is a command plus whatever it needs to be
 * told before it produces a program that runs. On Linux a Clang has to be told
 * which C++ standard library to use and, when that library is its own, where
 * the loader will find it afterwards. Report only the path and the caller is
 * left to guess the rest, which is how a build ends up hard-coding flags for
 * the machine it was written on — the thing Pickup exists to prevent.
 *
 * So the flags are part of the answer, and like every other answer here they
 * are proven rather than assumed: candidates are tried in order and the first
 * one that compiles, links AND runs is the one published. A recipe that was
 * never watched producing a working program is not a recipe.
 *
 * The order matters. libstdc++ is tried before libc++ because the two are not
 * ABI compatible, and libstdc++ is what everything else already compiled on
 * the machine is using. Falling back to the compiler's own library makes the
 * toolchain self-contained at the price of no longer being able to link
 * against the system's C++ libraries, which is a real cost and therefore a
 * last resort rather than a shortcut.
 */

/* Which C++ standard library the recipe settled on. */
typedef enum {
    stdlib_unknown,     /* nothing was needed, or nothing worked */
    stdlib_libstdcxx,   /* the GNU one, shared with the rest of the system */
    stdlib_libcxx,      /* the compiler's own, self-contained */
} cxx_stdlib;

/* Room for the flags a recipe may carry. Three is the most any candidate
   below uses; the rest is slack for candidates added later. */
#define RECIPE_MAX_FLAGS 8
#define RECIPE_FLAG_MAX  1024
#define RECIPE_MAX_DIRS  4

typedef struct {
    bool usable;        /* false when no candidate produced a running program */
    cxx_stdlib stdlib;

    /* What to add when compiling, and when linking. They overlap: -stdlib=
       belongs on both command lines, -Wl, only on the second. */
    char compile_flags[RECIPE_MAX_FLAGS][RECIPE_FLAG_MAX];
    size_t compile_count;
    char link_flags[RECIPE_MAX_FLAGS][RECIPE_FLAG_MAX];
    size_t link_count;

    /* Directories holding shared libraries the produced program needs at run
       time. Linking is not running: a caller that has to launch what it built,
       or ship it, needs these and cannot derive them from the flags. */
    char runtime_dirs[RECIPE_MAX_DIRS][PICKUP_PATHS_MAX];
    size_t runtime_count;
} link_recipe;

/* Find the flags that make `chain` build `lang`, by trying and checking.
   An unusable recipe is an answer: it means the toolchain cannot build that
   language at all, in any configuration Pickup knows how to offer. */
[[nodiscard]] link_recipe recipe_discover(const toolchain *chain, capability_lang lang);

/* The same, restricted to recipes that end up using `wanted`.

   `stdlib_unknown` takes the first configuration that works, which is the
   ordinary case. Naming one narrows the search instead of filtering its
   result: a toolchain whose preferred recipe uses libstdc++ may well have a
   working libc++ recipe further down the list, and a caller with an ABI
   constraint wants that one rather than a refusal. */
[[nodiscard]] link_recipe recipe_discover_for(const toolchain *chain, capability_lang lang,
                                              cxx_stdlib wanted);

/* The name published for a standard library: "libstdc++", "libc++", "". */
[[nodiscard]] const char *recipe_stdlib_name(cxx_stdlib stdlib);

/* Where `compiler` keeps its own libc++, asked of the compiler rather than
   guessed from its layout. False when it ships none, which is every GCC and
   every Clang that was built to use the system's library.

   `out` receives the directory the library sits in, which is what a run-time
   search path has to name. */
[[nodiscard]] bool recipe_own_libcxx_dir(const char *compiler, char *out, size_t out_size);

/* The flag in `recipe` that pins a GCC installation, or NULL when it names
   none. */
[[nodiscard]] const char *recipe_gcc_flag(const link_recipe *recipe);

/*
 * Make the C recipe stand on the same GCC installation as the C++ one.
 *
 * Clang borrows three things from a GCC — the C++ standard library, the
 * startup objects and libgcc — and only the first is about C++. So a toolchain
 * whose C++ driver was pinned to GCC 11 while its C driver goes on choosing
 * GCC 12 is one compiler standing on two: a project with sources in both
 * languages would link objects built against two different runtimes.
 *
 * The added flag is verified rather than assumed. C worked without it, and a
 * flag that turned out to break C would be a worse outcome than the mismatch
 * it was meant to fix, so it is kept only if the C recipe still compiles,
 * links and runs with it.
 *
 * `driver` is the C compiler to check against. False when nothing was changed.
 */
[[nodiscard]] bool recipe_align_gcc(const link_recipe *cxx, const char *driver,
                                    link_recipe *c);

/* Leave `recipe` beside the driver as a configuration file the compiler reads
   by itself, so the toolchain works when invoked directly and not only through
   a caller that read the recipe from `resolve`.

   `driver` is the compiler the file belongs to; Clang looks for <name>.cfg in
   its own directory without being told to. A convenience, not the contract:
   the answer Molto builds against is the one `resolve` publishes, which also
   covers the system compilers that cannot be written to.

   False when there is nothing worth writing or the file could not be created. */
[[nodiscard]] bool recipe_write_config(const char *driver, const link_recipe *recipe);

/* Which standard library `compiler` reaches for when invoked with `flags`.

   Read out of the library's own version macro through the preprocessor, the
   way probe_identify reads a compiler's identity: the default differs by
   platform — libstdc++ on Linux, libc++ on macOS — so it is one more thing
   that must be asked rather than known. */
[[nodiscard]] cxx_stdlib recipe_detect_stdlib(const char *compiler,
                                              const char *const *flags,
                                              size_t flag_count);

#endif /* PICKUP_RECIPE_H */
