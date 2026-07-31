#ifndef PICKUP_PROBE_H
#define PICKUP_PROBE_H

#include <stdbool.h>

#include <pickup/model/toolchain.h>

/* Ask the binary at `path` what it is, filling identity, version and target.
   Returns false if it is not a usable compiler.

   Identity comes from the compiler's own predefined macros, never from its
   filename: `gcc` on macOS is Clang, and `cc` is whatever the distribution
   decided. Only asking gives a truthful answer. */
[[nodiscard]] bool probe_identify(const char *path, toolchain *out);

/* Find the C++ driver that belongs with the C compiler in `chain`, verify it
   runs, and record it. Leaves `cxx_path` empty when there is none, rather than
   assuming a name that may not exist. */
void probe_find_cxx_driver(toolchain *chain);

/* Probe and record which C and C++ features `chain` actually compiles. */
void probe_capabilities(toolchain *chain);

#endif /* PICKUP_PROBE_H */
