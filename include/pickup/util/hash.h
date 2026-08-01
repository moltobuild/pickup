#ifndef PICKUP_HASH_H
#define PICKUP_HASH_H

/*
 * FNV-1a, 64 bits.
 *
 * It exists to notice that something changed, not to resist anyone trying to
 * forge a collision, so a short non-cryptographic hash is the right tool.
 */

/* The value a hash starts from. */
#define HASH_INIT 1469598103934665603ULL

/* Fold `text` into `hash` and return the result. `text` must not be NULL.

   An item terminator is mixed in with it, so a sequence is hashed
   unambiguously: "ab" then "c" does not collide with "a" then "bc". */
[[nodiscard]] unsigned long long hash_add(unsigned long long hash, const char *text);

#endif /* PICKUP_HASH_H */
