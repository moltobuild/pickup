#ifndef PICKUP_PREFERENCE_SERVICE_H
#define PICKUP_PREFERENCE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/model/toolchain.h>

/*
 * The choices a user made, kept across runs.
 *
 * Everything else Pickup writes is either regenerable — the probed inventory,
 * the release index — or is the thing that was installed. This is neither: it
 * is a decision, and rebuilding it is not something Pickup can do on its own.
 * So it lives beside those directories rather than inside the cache, where a
 * `pickup scan` or a cleared cache would silently discard it.
 *
 *   <home>/config.toml
 *
 * TOML because that is what the ecosystem already reads: the file a user may
 * want to edit by hand, or commit to a repository, is in the same shape as
 * every answer Pickup publishes.
 *
 * Only the keys written here are understood on the way back in. Anything else
 * in the file is preserved untouched rather than parsed, so a key added by a
 * later version — or by hand — survives a write from this one.
 */

/* Room for a stored preference. The longest is a toolchain identity. */
#define PREFERENCE_VALUE_MAX PICKUP_ID_MAX

/*
 * The default toolchain's identity, as `pickup default` stored it.
 *
 * False when none was ever set, which is not an error: it is the state every
 * machine starts in, and the one in which `resolve` ranks candidates for
 * itself.
 *
 * What comes back is an identity and nothing more. Whether a toolchain of that
 * identity is still installed is a question for the inventory, and asking it
 * here would mean a scan every time the file is read.
 */
[[nodiscard]] bool preference_default_get(char *out, size_t out_size);

/* Record `id` as the default. Creates the home and the file if neither is
   there yet. False if it could not be written, which must be reported: a
   preference that was silently not stored is worse than one that was
   refused. */
[[nodiscard]] bool preference_default_set(const char *id);

/* Forget the default. Succeeds when there was none to forget, so clearing
   twice is not an error. */
[[nodiscard]] bool preference_default_clear(void);

#endif /* PICKUP_PREFERENCE_SERVICE_H */
