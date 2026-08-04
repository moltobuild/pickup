#ifndef PICKUP_PATHS_SERVICE_H
#define PICKUP_PATHS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where Pickup keeps everything it writes.
 *
 * All of it lives under one directory the user owns, so installing a toolchain
 * never needs administrator rights and removing one is deleting a folder.
 * `PICKUP_HOME` relocates the lot, which is also what lets the tests run
 * without touching a real home directory.
 *
 *   <home>/cache/       regenerable: the probed inventory, the release index
 *   <home>/downloads/   archives in flight, removed once installed
 *   <home>/toolchains/  what was installed, one directory each
 *
 * The first two can be deleted at any time and cost a rescan. The third is
 * what the downloads were for.
 */

/* Environment variable that relocates all of it. */
#define PICKUP_HOME_ENV "PICKUP_HOME"

/* Default location, relative to the user's home. */
#define PICKUP_HOME_DIRNAME ".pickup"

/* Room for any of the paths below. */
#define PICKUP_PATHS_MAX 4096

/* The root: $PICKUP_HOME, or ~/.pickup. False if neither is known. */
[[nodiscard]] bool paths_home(char *out, size_t out_size);

/* Where installed toolchains live, one directory each. */
[[nodiscard]] bool paths_toolchains(char *out, size_t out_size);

/* Where installed tools live — a formatter, a linter — one directory each.

   Kept apart from the toolchains because they are not toolchains: nothing
   resolves against them, `list` does not report them, and a scan that treated
   a clang-format as a compiler candidate would waste a probe on every run. */
[[nodiscard]] bool paths_tools(char *out, size_t out_size);

/* Where answers worth keeping but never worth trusting blindly are stored:
   the probed inventory and the index of published releases. Deleting any of it
   costs time, never correctness. */
[[nodiscard]] bool paths_cache(char *out, size_t out_size);

/* Where archives are downloaded before they are verified and unpacked. Holds
   only files in flight; anything that finishes installing is removed. */
[[nodiscard]] bool paths_downloads(char *out, size_t out_size);

/* The directory a toolchain of this identity is installed into, named so that
   two versions, or the same version for two targets, never collide. */
[[nodiscard]] bool paths_toolchain_dir(const char *vendor, const char *version,
                                       const char *target, char *out, size_t out_size);

#endif /* PICKUP_PATHS_SERVICE_H */
