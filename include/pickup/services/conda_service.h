#ifndef PICKUP_CONDA_SERVICE_H
#define PICKUP_CONDA_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Installing a conda package into a prefix Pickup owns.
 *
 * Unpacking is only half of it. A conda package is built somewhere else, under
 * a long placeholder path, and files that had to record where they were built
 * still carry it: compiler specs, the sysroot a driver reaches for, scripts
 * with a shebang. Unpacked and left alone, a compiler looks for its own parts
 * under a directory that was never on this machine.
 *
 * So the build prefix is rewritten to the real one, which is what the conda
 * tools do and what `info/paths.json` exists to make possible: it names every
 * file that carries the placeholder, and whether it does so as text or inside
 * a binary. The two are not rewritten the same way — a binary must not change
 * length, so what is replaced is padded back out with NUL bytes.
 *
 * None of this is trusted to have worked. What comes out is asked to compile,
 * link and run before it is adopted, the same test Pickup turns on its own
 * installer (spec.md section 11).
 */

/* Unpack `package` into `prefix` and rewrite the build prefix out of it.

   `prefix` must already exist. Packages of one toolchain are unpacked into the
   same prefix, one after another, which is what conda does and what makes the
   parts of a compiler find each other. */
[[nodiscard]] bool conda_install_package(const char *package, const char *prefix);

/* Replace `placeholder` with `prefix` in the file at `path`.

   `binary` selects how: text is substituted plainly and the file may change
   length, while a binary keeps its length and the replacement is padded with
   NUL bytes. Exposed for its own sake because it is the part that is easy to
   get wrong and the part worth testing directly.

   False when a binary's prefix would have to grow, which cannot be done
   without moving everything that follows it. */
[[nodiscard]] bool conda_rewrite_prefix(const char *path, const char *placeholder,
                                        const char *prefix, bool binary);

/* Rewrite every file that `paths_json` says carries the build prefix, under
   `prefix`. Split from the unpacking so it can be tested against a recorded
   metadata file rather than a downloaded package. */
[[nodiscard]] bool conda_rewrite_all(const char *paths_json, const char *prefix,
                                     size_t *rewritten);

#endif /* PICKUP_CONDA_SERVICE_H */
