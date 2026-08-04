#ifndef PICKUP_GCC_INSTALL_H
#define PICKUP_GCC_INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pickup/services/paths_service.h>

/*
 * Which GCC installation a Clang driver is standing on.
 *
 * Clang ships no C++ standard library and no startup files of its own on Linux.
 * It borrows them from a GCC installation it locates at run time, and it takes
 * three separate things from it: the libstdc++ headers, the startup objects
 * (crtbeginS.o, crtendS.o) and libgcc. Only the first has an alternative — the
 * compiler's own libc++ — which is why a GCC that is missing its C++ half
 * breaks `#include <iostream>` while C keeps compiling perfectly.
 *
 * Which one it picked is a decision the driver makes internally and prints
 * nowhere except under `-v`. So it is read from there: the compiler describing
 * its own choice, the same way probe_identify reads its identity out of the
 * preprocessor rather than guessing from a file name.
 *
 * Whether an installation actually carries libstdc++ is then checked on disk.
 * The version number does not answer it: on the machine this was written for,
 * GCC 12 is installed without its C++ half and GCC 11 beside it is complete.
 */

/* Most GCC installations one machine is expected to carry. Distributions ship
   two or three; the room is for the ones that keep several around. */
#define GCC_INSTALL_MAX 12

/* No installation was selected. */
#define GCC_INSTALL_NONE SIZE_MAX

typedef struct {
    char path[PICKUP_PATHS_MAX];  /* e.g. /usr/lib/gcc/x86_64-linux-gnu/12 */
    bool has_libstdcxx;           /* checked on disk, never inferred */
} gcc_install;

typedef struct {
    gcc_install items[GCC_INSTALL_MAX];
    size_t count;
    size_t selected;  /* index into items, or GCC_INSTALL_NONE */
} gcc_install_list;

/*
 * Ask `clang_path` which GCC installations it found and which it chose.
 *
 * `ignore_config` decides which of two different questions is being asked, and
 * they are not interchangeable:
 *
 *   false — what this driver actually does when invoked. A configuration file
 *           beside it pinning a GCC is part of that, and a diagnosis that
 *           ignored it would describe a compiler nobody is running.
 *   true  — what this machine offers. A pinned GCC stops the driver from
 *           enumerating the others at all, so anything choosing between them
 *           has to ask with the file out of the way, or it will only ever see
 *           the choice already made.
 *
 * False if the driver could not be run at all. A compiler that reports none —
 * every non-Clang one — yields an empty list, which is an answer, not a
 * failure.
 */
[[nodiscard]] bool gcc_install_query(const char *clang_path, bool ignore_config,
                                     gcc_install_list *out);

/* Read the same out of text `clang -v` already produced.

   Split from the call so the parsing can be tested against a recorded answer,
   the way llvm_parse_releases is tested without a network. */
[[nodiscard]] bool gcc_install_parse(const char *verbose_output, gcc_install_list *out);

/* True if `directory` carries libstdc++ headers, checked by looking for them.

   Clang reaches them relative to the installation directory, so the same
   relative walk is used here rather than a hard-coded /usr/include. */
[[nodiscard]] bool gcc_install_has_libstdcxx(const char *directory);

/* The installation in `list` that carries libstdc++ and has the highest
   version. False when none of them does, which is the state that leaves a
   Clang unable to compile C++ at all. */
[[nodiscard]] bool gcc_install_best(const gcc_install_list *list, gcc_install *out);

#endif /* PICKUP_GCC_INSTALL_H */
