#ifndef PICKUP_INVENTORY_SERVICE_H
#define PICKUP_INVENTORY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/model/toolchain.h>

/* The toolchains found on this machine. */
typedef struct {
    toolchain *items;
    size_t count;
    size_t capacity;
} inventory;

/* Discover every usable toolchain: scan PATH for candidates, ask each one what
   it is, and probe what it can actually compile. Ordered deterministically
   (vendor, then descending version) so the same machine always reports the
   same list. Returns false on allocation failure. */
[[nodiscard]] bool inventory_discover(inventory *out);

/* The inventory as commands should obtain it: served from the cache when it
   still describes this machine, discovered and cached otherwise. `refresh`
   forces a full rescan. */
[[nodiscard]] bool inventory_load(inventory *out, bool refresh);

/* Told how far the probing has got: `done` candidates asked of `total`.

   Probing is the slow part of every inventory — each candidate is made to
   compile several programs — and a command that says nothing for that long
   looks like one that has died. Called before each candidate and once more when
   the last has answered. A cached inventory probes nothing and so reports
   nothing. */
typedef void (*inventory_watch)(size_t done, size_t total, void *context);

/* `inventory_load`, watched. A NULL `watch` is the unwatched load. */
[[nodiscard]] bool inventory_load_watched(inventory *out, bool refresh,
                                          inventory_watch watch, void *context);

/* Append a toolchain. Exposed so the cache can rebuild an inventory it read. */
[[nodiscard]] bool inventory_append(inventory *list, const toolchain *chain);

/* Release an inventory. Safe on an empty one. */
void inventory_free(inventory *list);

/* Find a toolchain by name ("gcc-12") or by path. NULL if there is none. */
[[nodiscard]] const toolchain *inventory_find(const inventory *list, const char *name);

#endif /* PICKUP_INVENTORY_SERVICE_H */
