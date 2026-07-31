#ifndef PICKUP_CAPABILITY_H
#define PICKUP_CAPABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What a compiler can actually do.
 *
 * A compiler that accepts a flag has not necessarily implemented the standard
 * behind it: gcc 9 accepts `-std=c2x` and rejects `[[nodiscard]]`. So Pickup
 * never asks "do you take this flag?" — it hands over a program that uses the
 * feature and looks at whether it compiled. That is the only answer that does
 * not lie (spec.md section 7).
 */

/* The language a feature belongs to. A compiler is probed once per language. */
typedef enum {
    lang_c,
    lang_cxx,
} capability_lang;

/* One provable feature. Adding a feature means adding a table entry and
   nothing else. */
typedef struct {
    const char *id;       /* stable identifier, e.g. "attr_nodiscard" */
    capability_lang lang;
    const char *standard; /* the -std= value that introduced it, e.g. "c2x" */
    const char *program;  /* a minimal program that uses the feature */
} capability;

/* Room for the feature bitset. Raising it is the only limit on the catalog. */
#define PICKUP_MAX_CAPABILITIES 64

/* Which features a compiler proved. One bit per catalog entry. */
typedef struct {
    uint64_t bits;
} capability_set;

/* The catalog, and its size. */
[[nodiscard]] const capability *capability_catalog(size_t *count);

/* Index of `id` in the catalog, or SIZE_MAX if unknown. */
[[nodiscard]] size_t capability_index(const char *id);

/* Test, set and clear a feature by catalog index. */
[[nodiscard]] bool capability_set_has(capability_set set, size_t index);
void capability_set_add(capability_set *set, size_t index);

/* True if every feature in `required` is present in `available`. */
[[nodiscard]] bool capability_set_contains(capability_set available, capability_set required);

/* The features the catalog attributes to `standard` in `lang`. Used to turn
   "supports c2x" into the concrete set that has to be proven. */
[[nodiscard]] capability_set capability_set_for_standard(capability_lang lang,
                                                         const char *standard);

/* Probe `compiler` for every feature of `lang` by compiling each program, and
   return what it proved. `compiler` is the binary to run. */
[[nodiscard]] capability_set capability_probe(const char *compiler, capability_lang lang);

/* True if `compiler` accepts `-std=<standard>` for `lang`, proven by compiling
   a trivial program in that mode.

   Accepting a standard is not the same as implementing it — gcc 9 accepts
   `-std=c2x` and rejects `[[nodiscard]]`. This answers only "can it be invoked
   this way?"; what it can then compile is a question for the feature probes. */
[[nodiscard]] bool capability_accepts_standard(const char *compiler, capability_lang lang,
                                               const char *standard);

#endif /* PICKUP_CAPABILITY_H */
