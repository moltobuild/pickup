#ifndef PICKUP_PATHS_SERVICE_H
#define PICKUP_PATHS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where Pickup keeps everything it writes.
 *
 * All of it lives in directories the user owns, so installing a toolchain never
 * needs administrator rights and removing one is deleting a folder. What is
 * kept splits three ways, by what losing it would cost, and each part goes
 * where the platform says that kind of thing belongs:
 *
 *   config  the choices the user made (config.toml)       never regenerable
 *   data    toolchains/ and tools/, what was installed     costs a download
 *   cache   the probed inventory, the registry indexes,    costs a rescan
 *           and downloads/, archives in flight
 *
 * On Linux and macOS those are the XDG base directories, macOS included: a
 * command-line tool there is found in ~/.config like everywhere else, and
 * ~/Library is where an application bundle's state goes.
 *
 *   config  $XDG_CONFIG_HOME/pickup   (~/.config/pickup)
 *   data    $XDG_DATA_HOME/pickup     (~/.local/share/pickup)
 *   cache   $XDG_CACHE_HOME/pickup    (~/.cache/pickup)
 *
 * On Windows all three are under %APPDATA%\pickup:
 *
 *   config  %APPDATA%\pickup                 config.toml
 *   data    %APPDATA%\pickup                 toolchains\ and tools\
 *   cache   %APPDATA%\pickup\cache           downloads\ inside it
 *
 * `PICKUP_HOME` puts the lot back under one directory, in the layout every
 * version before this one used — which is also what lets the tests run without
 * touching a real home directory:
 *
 *   <home>/config.toml  <home>/toolchains/  <home>/tools/
 *   <home>/cache/       <home>/downloads/
 */

/* Environment variable that relocates all of it. */
#define PICKUP_HOME_ENV "PICKUP_HOME"

/* Where everything lived before the split, relative to the user's home. Read
   now only to move what is in it (see migrate_service.h). */
#define PICKUP_HOME_DIRNAME ".pickup"

/* The directory Pickup owns inside each platform base directory. */
#define PICKUP_DIRNAME "pickup"

/* Room for any of the paths below. */
#define PICKUP_PATHS_MAX 4096

/* True when $PICKUP_HOME is set, and everything lives under it. */
[[nodiscard]] bool paths_relocated(void);

/*
 * The single-directory home: $PICKUP_HOME when it is set, and otherwise
 * ~/.pickup (%USERPROFILE%\.pickup), the legacy location nothing new is
 * written to. False if neither is known.
 *
 * Kept under its old name because a relocated home still is exactly this, and
 * the legacy one is what the migration empties.
 */
[[nodiscard]] bool paths_home(char *out, size_t out_size);

/* The legacy ~/.pickup, whatever $PICKUP_HOME says. */
[[nodiscard]] bool paths_legacy_home(char *out, size_t out_size);

/* Where the user's choices are kept: the directory holding config.toml. */
[[nodiscard]] bool paths_config(char *out, size_t out_size);

/* Where what was installed is kept: the parent of toolchains/ and tools/. */
[[nodiscard]] bool paths_data(char *out, size_t out_size);

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
[[nodiscard]] bool paths_toolchain_dir(const char *vendor, const char *version, const char *target,
                                       char *out, size_t out_size);

/*
 * The installed directory that `path` belongs to: the first level under
 * the toolchains directory containing it.
 *
 * Read from the path rather than rebuilt with paths_toolchain_dir, because the
 * two can disagree. The name is composed at install time from what the
 * unpacked compiler said it was, and a compiler that reports a different
 * target after an upgrade would be looked for under a directory it was never
 * in — and the answer would be a path to delete.
 *
 * False when `path` is not under there at all, which is exactly what a
 * compiler Pickup did not install looks like. Removing one of those is the
 * package manager's business.
 */
[[nodiscard]] bool paths_owning_toolchain(const char *path, char *out, size_t out_size);

#endif /* PICKUP_PATHS_SERVICE_H */
