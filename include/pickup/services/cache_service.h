#ifndef PICKUP_CACHE_SERVICE_H
#define PICKUP_CACHE_SERVICE_H

#include <stdbool.h>

#include <pickup/services/inventory_service.h>
#include <pickup/util/str_list.h>

/*
 * Probing is expensive: every compiler is invoked once per feature. The cache
 * keeps the answers so that only the first query pays for them.
 *
 * It is never trusted blindly. A cached entry is valid while the binary it
 * describes is unchanged, and the whole file is discarded when anything looks
 * off. Rescanning is always correct; trusting stale state is not.
 */

/* Load the cached inventory into `out`, but only if it still describes this
   machine: `candidates` (a fresh, cheap scan of PATH) must match the cached
   set exactly, and every binary must be unchanged since it was probed.
   Returns false when the cache is absent, stale, or unreadable — in which case
   `out` is left empty and the caller should discover from scratch. */
[[nodiscard]] bool cache_load(const str_list *candidates, inventory *out);

/* Write `list` to the cache, along with a fingerprint of `candidates` so a
   later load can tell whether the machine still looks the same. Returns false
   if it could not be stored, which costs a rescan next time and nothing else. */
bool cache_store(const str_list *candidates, const inventory *list);

/* Delete the cache file. Used by an explicit refresh. */
void cache_discard(void);

#endif /* PICKUP_CACHE_SERVICE_H */
