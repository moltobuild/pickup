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

/*
 * Put an inventory into the form every caller expects: each compiler once,
 * named by its identity, in a fixed order.
 *
 * A single GCC answers to `cc`, `gcc-9`, `c89-gcc` and `g++-9`; those are one
 * toolchain, and listing them separately says four things where there is one.
 * Collapsing them keeps everything that was proven about each spelling, since
 * they were probed independently and a `c++` alone reports no C++ features at
 * all.
 *
 * Exposed because this is the part worth testing, and it cannot be tested
 * through a discovery that depends on whichever compilers the machine running
 * the tests happens to have.
 */
void inventory_settle(inventory *list);

/* Release an inventory. Safe on an empty one. */
void inventory_free(inventory *list);

/* Find a toolchain by identity ("gcc@12.3.0"), by a shorter form of it
   ("gcc@12", "gcc@latest", "gcc"), or by path. The highest version that
   matches, since the inventory is ordered newest first. NULL if there is
   none. */
[[nodiscard]] const toolchain *inventory_find(const inventory *list, const char *name);

/* How many toolchains `name` names. More than one means the query was too
   loose to identify anything, which is worth telling a user rather than
   silently answering with whichever came first. */
[[nodiscard]] size_t inventory_count_matching(const inventory *list, const char *name);

#endif /* PICKUP_INVENTORY_SERVICE_H */
