#ifndef PICKUP_PROBE_PROGRESS_H
#define PICKUP_PROBE_PROGRESS_H

#include <stdbool.h>

#include <pickup/services/inventory_service.h>

/*
 * The inventory, with the probing made visible.
 *
 * Every command that has to know what this machine carries needs it the same
 * way: probing is the slow part, and a command that says nothing for that long
 * looks like one that has died. What differs between them is what they print
 * afterwards, which is their own business and stays there.
 */

/* Load the inventory, drawing a bar for the probing. `refresh` forces a full
   rescan, as it does for inventory_load.

   The bar goes to stderr, never to stdout: the table, and the TOML in
   particular, is the answer, and a progress line has no business in a file that
   answer was redirected into. It appears only on a terminal, and only when
   there is probing to report — a cached inventory draws nothing — and the line
   is wiped before this returns. */
[[nodiscard]] bool probe_progress_load(inventory *out, bool refresh);

#endif /* PICKUP_PROBE_PROGRESS_H */
