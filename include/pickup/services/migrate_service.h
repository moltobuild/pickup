#ifndef PICKUP_MIGRATE_SERVICE_H
#define PICKUP_MIGRATE_SERVICE_H

#include <pickup/services/paths_service.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Moving what an older Pickup left in ~/.pickup to where it belongs now.
 *
 * Everything used to live under one directory; it now lives in the platform's
 * directories for configuration, data and cache (paths_service.h). A machine
 * that upgraded still has its toolchains, tools and preferences in the old
 * place, and the first run of this version moves them:
 *
 *   ~/.pickup/toolchains/<name>   ->  <data>/toolchains/<name>
 *   ~/.pickup/tools/<name>        ->  <data>/tools/<name>
 *   ~/.pickup/config.toml         ->  <config>/config.toml
 *   ~/.pickup/cache, downloads    ->  dropped; rebuilt on demand
 *
 * Each entry is renamed, never copied, so a toolchain is either wholly in the
 * old place or wholly in the new one. A Clang Pickup configured has a .cfg
 * beside its driver that names directories by absolute path, and those are
 * rewritten to the new location as part of the move (recipe_relocate_config):
 * moving the directory alone would leave a compiler whose programs cannot find
 * their runtime.
 *
 * Nothing is overwritten and nothing that did not move is deleted. Whatever
 * cannot be moved — a name already taken at the destination, a destination on
 * another filesystem — stays where it is, the old directory stays with it, and
 * a note is left there so the next run does not try, and fail, all over again.
 * The old directory is removed only once it is empty.
 *
 * Never run while PICKUP_HOME is set: that is a user, or a test, who chose
 * where everything goes.
 */

/* The note left in a legacy directory that could not be emptied. Its presence
   is what stops the migration from being attempted again; deleting it asks for
   another attempt. */
#define MIGRATE_MARKER_FILENAME "MIGRATED.txt"

/* How many of the entries that could not move are named in a report. */
#define MIGRATE_LEFT_MAX 8

/* Room for one entry's name. */
#define MIGRATE_NAME_MAX 256

typedef struct {
    /* False when there was nothing to do: no legacy directory, one already
       marked as attempted, or a relocated home. Nothing else is set then. */
    bool attempted;

    size_t toolchains_moved;
    size_t tools_moved;
    bool preferences_moved;

    /* What is still in the legacy directory, named relative to it. `left_count`
       counts all of it; only the first MIGRATE_LEFT_MAX are named. */
    size_t left_count;
    char left[MIGRATE_LEFT_MAX][MIGRATE_NAME_MAX];

    /* True once the legacy directory is gone. */
    bool legacy_removed;

    char legacy[PICKUP_PATHS_MAX];
    char data[PICKUP_PATHS_MAX];
    char config[PICKUP_PATHS_MAX];
} migrate_report;

/* Move the legacy directory's contents, once. Safe to call on every run: it
   costs one stat when there is nothing to do. */
[[nodiscard]] migrate_report migrate_legacy_home(void);

/* Say what happened, in a line or two, on `stream`. Silent when nothing was
   attempted. */
void migrate_announce(const migrate_report *report, FILE *stream);

#endif /* PICKUP_MIGRATE_SERVICE_H */
