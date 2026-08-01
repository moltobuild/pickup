#include <pickup/util/hash.h>

/* The FNV-1a 64-bit multiplier. */
#define HASH_PRIME 1099511628211ULL

/* Mixed in after each item so that adjacent items cannot be mistaken for one
   longer item. */
#define ITEM_TERMINATOR '\0'

static unsigned long long fold(unsigned long long hash, unsigned char byte) {
    hash ^= byte;
    return hash * HASH_PRIME;
}

unsigned long long hash_add(unsigned long long hash, const char *text) {
    for (const char *cursor = text; *cursor != '\0'; cursor++)
        hash = fold(hash, (unsigned char)*cursor);
    return fold(hash, ITEM_TERMINATOR);
}
