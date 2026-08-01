#ifndef PICKUP_PATHS_SERVICE_H
#define PICKUP_PATHS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where Pickup keeps what it installs.
 *
 * Everything lives under one directory the user owns, so installing a
 * toolchain never needs administrator rights and removing one is deleting a
 * folder. `PICKUP_HOME` overrides the location, which is also what lets the
 * tests exercise installation without touching a real home directory.
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

/* Where archives are downloaded before they are verified and unpacked. */
[[nodiscard]] bool paths_downloads(char *out, size_t out_size);

/* The directory a toolchain of this identity is installed into, named so that
   two versions, or the same version for two targets, never collide. */
[[nodiscard]] bool paths_toolchain_dir(const char *vendor, const char *version,
                                       const char *target, char *out, size_t out_size);

#endif /* PICKUP_PATHS_SERVICE_H */
