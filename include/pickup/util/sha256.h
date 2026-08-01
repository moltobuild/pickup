#ifndef PICKUP_SHA256_H
#define PICKUP_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * SHA-256 (FIPS 180-4).
 *
 * Pickup verifies a downloaded toolchain against the digest its source
 * published before unpacking it, and the archives run to gigabytes, so the
 * interface is incremental: nothing is ever held in memory whole.
 */

/* Bytes in a digest, and in its hex form counting the terminator. */
#define SHA256_DIGEST_SIZE 32
#define SHA256_HEX_SIZE    65

/* How much of a file is read at a time when hashing it. */
#define SHA256_CHUNK_SIZE 65536

typedef struct {
    uint32_t state[8];
    uint64_t length;                   /* total bytes fed in */
    unsigned char block[64];
    size_t buffered;
} sha256_state;

/* Begin a digest. */
void sha256_init(sha256_state *state);

/* Feed in more data. Call as many times as needed. */
void sha256_update(sha256_state *state, const void *data, size_t length);

/* Finish, writing SHA256_HEX_SIZE bytes of lowercase hex into `hex_out`. The
   state must not be used again afterwards. */
void sha256_finish(sha256_state *state, char *hex_out);

/* Hash a whole file. False if it could not be read; `hex_out` needs room for
   SHA256_HEX_SIZE bytes. */
[[nodiscard]] bool sha256_file(const char *path, char *hex_out);

/* Compare two hex digests without regard to case, and without giving up early,
   so the comparison cannot be timed. Either being NULL is false. */
[[nodiscard]] bool sha256_hex_equal(const char *left, const char *right);

#endif /* PICKUP_SHA256_H */
