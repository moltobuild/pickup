#include <moltest.h>

#include <pickup/services/fs_service.h>
#include <pickup/util/sha256.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Digest of `text` as a convenience for the vector tests. */
static void digest_of(const char *text, char *hex_out) {
    sha256_state state;
    sha256_init(&state);
    sha256_update(&state, text, strlen(text));
    sha256_finish(&state, hex_out);
}

MOLTEST(sha256_matches_the_published_vectors) {
    char hex[SHA256_HEX_SIZE];

    /* The FIPS 180-4 examples. Getting these right is the whole point: a hash
       that is subtly wrong would reject every download instead of verifying
       it. */
    digest_of("", hex);
    EXPECT_STREQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);

    digest_of("abc", hex);
    EXPECT_STREQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);

    digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    EXPECT_STREQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", hex);
}

MOLTEST(sha256_handles_a_message_spanning_many_blocks) {
    /* A million 'a', the third FIPS vector: exercises the block loop and a
       length that no longer fits in a byte count. */
    sha256_state state;
    sha256_init(&state);

    static char chunk[1000];
    memset(chunk, 'a', sizeof chunk);
    for (int i = 0; i < 1000; i++)
        sha256_update(&state, chunk, sizeof chunk);

    char hex[SHA256_HEX_SIZE];
    sha256_finish(&state, hex);
    EXPECT_STREQ("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", hex);
}

MOLTEST(sha256_is_unaffected_by_how_the_data_is_split) {
    /* Downloads arrive in arbitrary pieces; the digest must not depend on
       where the boundaries fell. */
    const char *message = "the quick brown fox jumps over the lazy dog, repeatedly";

    char whole[SHA256_HEX_SIZE];
    digest_of(message, whole);

    sha256_state state;
    sha256_init(&state);
    for (size_t i = 0; message[i] != '\0'; i++)
        sha256_update(&state, message + i, 1);
    char byte_by_byte[SHA256_HEX_SIZE];
    sha256_finish(&state, byte_by_byte);

    EXPECT_STREQ(whole, byte_by_byte);
}

MOLTEST(sha256_hashes_a_file) {
    char path[] = "/tmp/pickup_sha_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    ASSERT_TRUE(fs_write_file(path, "abc"));

    char hex[SHA256_HEX_SIZE];
    EXPECT_TRUE(sha256_file(path, hex));
    EXPECT_STREQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);

    remove(path);
    EXPECT_FALSE(sha256_file("/nonexistent/archive.tar.xz", hex));
}

MOLTEST(sha256_compares_digests_case_insensitively) {
    const char *lower = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const char *upper = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    const char *other = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae";

    EXPECT_TRUE(sha256_hex_equal(lower, upper));
    EXPECT_FALSE(sha256_hex_equal(lower, other));
    EXPECT_FALSE(sha256_hex_equal(lower, "ba7816bf"));
    EXPECT_FALSE(sha256_hex_equal(NULL, lower));
    EXPECT_FALSE(sha256_hex_equal(lower, NULL));
}

/* Collects what a hash reported as it went. */
typedef struct { long long last_done; long long total; int calls; } progress_record;

static void record_progress(long long done, long long total, void *context) {
    progress_record *record = context;
    record->last_done = done;
    record->total = total;
    record->calls++;
}

MOLTEST(sha256_reports_progress_while_it_hashes) {
    char path[] = "/tmp/pickup_sha_watch_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    /* Big enough to span several read chunks, so progress has somewhere to
       go. */
    FILE *file = fopen(path, "wb");
    ASSERT_TRUE(file != NULL);
    static char block[65536];
    memset(block, 'a', sizeof block);
    for (int i = 0; i < 4; i++)
        fwrite(block, 1, sizeof block, file);
    fclose(file);

    progress_record seen = { 0, 0, 0 };
    char hex[SHA256_HEX_SIZE];
    EXPECT_TRUE(sha256_file_watched(path, hex, record_progress, &seen));

    /* Reported as a fraction of a known total, growing to all of it. */
    EXPECT_TRUE(seen.calls > 1);
    EXPECT_TRUE(seen.total == 4 * 65536);
    EXPECT_TRUE(seen.last_done == seen.total);

    /* And the digest is the same one the unwatched call produces. */
    char plain[SHA256_HEX_SIZE];
    EXPECT_TRUE(sha256_file(path, plain));
    EXPECT_STREQ(plain, hex);

    remove(path);
}
