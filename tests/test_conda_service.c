#include <moltest.h>

#include <pickup/services/conda_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/util/zip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Rewriting the build prefix is the part of installing a conda package that
 * has to be exactly right and cannot be checked by looking.
 *
 * A package is built somewhere else, under a long placeholder path, and files
 * that had to record where they were built still carry it. Text and binaries
 * are not rewritten the same way: a binary is full of offsets into itself, so
 * it must not change length, and what is replaced is padded back out with NUL
 * bytes. Get that wrong and the compiler still unpacks, still runs, and looks
 * for its own parts in a directory that was never on this machine.
 */

typedef struct {
    char root[64];
    char file[PICKUP_PATHS_MAX];
} fixture;

static bool fixture_setup(fixture *f) {
    snprintf(f->root, sizeof f->root, "%s", "/tmp/pickup_conda_t_XXXXXX");
    if (mkdtemp(f->root) == NULL)
        return false;
    return fs_format_path(f->file, sizeof f->file, "%s/subject", f->root);
}

static void fixture_teardown(fixture *f) {
    (void)fs_remove_tree(f->root);
}

/* The long path a package records at build time. */
#define PLACEHOLDER "/opt/conda/conda-bld/placehold_placehold_placehold"
#define REAL_PREFIX "/home/user/.pickup/toolchains/gcc"

MOLTEST(conda_rewrites_text_and_lets_it_change_length) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    ASSERT_TRUE(fs_write_file(&f.file[0],
        "#!/bin/sh\n"
        "export SYSROOT=" PLACEHOLDER "/sysroot\n"
        "exec " PLACEHOLDER "/bin/gcc \"$@\"\n"));

    ASSERT_TRUE(conda_rewrite_prefix(f.file, PLACEHOLDER, REAL_PREFIX, false));

    char *after = fs_read_file(f.file);
    ASSERT_TRUE(after != NULL);
    /* Every occurrence, not just the first: a script that had one of them
       rewritten and one left behind is worse than one left alone. */
    EXPECT_TRUE(strstr(after, PLACEHOLDER) == NULL);
    EXPECT_TRUE(strstr(after, REAL_PREFIX "/sysroot") != NULL);
    EXPECT_TRUE(strstr(after, REAL_PREFIX "/bin/gcc") != NULL);
    free(after);

    fixture_teardown(&f);
}

MOLTEST(conda_keeps_a_binary_exactly_as_long_as_it_was) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    /* A stand-in for an ELF: a NUL-terminated path in the middle of bytes
       that must not move. */
    unsigned char before[256];
    memset(before, 0xAB, sizeof before);
    size_t at = 64;
    memcpy(before + at, PLACEHOLDER, sizeof PLACEHOLDER);
    ASSERT_TRUE(fs_write_bytes(f.file, before, sizeof before));

    ASSERT_TRUE(conda_rewrite_prefix(f.file, PLACEHOLDER, REAL_PREFIX, true));

    size_t size = 0;
    unsigned char *after = fs_read_bytes(f.file, &size);
    ASSERT_TRUE(after != NULL);

    /* The length is the whole point: every offset in a real binary would be
       wrong if this changed. */
    EXPECT_EQ((int)sizeof before, (int)size);
    EXPECT_TRUE(memcmp(after + at, REAL_PREFIX, strlen(REAL_PREFIX)) == 0);
    /* What the shorter replacement left behind is NUL, which also terminates
       the string the program will read. */
    EXPECT_EQ(0, (int)after[at + strlen(REAL_PREFIX)]);
    /* And nothing outside the placeholder was touched. */
    EXPECT_EQ(0xAB, (int)after[at - 1]);
    EXPECT_EQ(0xAB, (int)after[at + sizeof PLACEHOLDER]);

    free(after);
    fixture_teardown(&f);
}

MOLTEST(conda_keeps_whole_the_path_that_follows_the_prefix) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    /*
     * The failure this guards against, found by installing a real GCC.
     *
     * A linker carries its own script embedded as text, holding entries like
     * SEARCH_DIR("=<prefix>/lib"). Padding with NUL immediately after the
     * replacement cuts that string in half — the "/lib" and the closing quote
     * are lost — and the linker then refuses to parse its own script with a
     * syntax error, while every other check still passes.
     *
     * The padding belongs at the end of the string, not in the middle of it.
     */
    const char script[] = "SEARCH_DIR(\"=" PLACEHOLDER "/lib\");";
    unsigned char before[512];
    memset(before, 0, sizeof before);
    memcpy(before, script, sizeof script);
    ASSERT_TRUE(fs_write_bytes(f.file, before, sizeof before));

    ASSERT_TRUE(conda_rewrite_prefix(f.file, PLACEHOLDER, REAL_PREFIX, true));

    size_t size = 0;
    unsigned char *after = fs_read_bytes(f.file, &size);
    ASSERT_TRUE(after != NULL);
    EXPECT_EQ((int)sizeof before, (int)size);

    /* The whole entry survives, closing quote and all. */
    EXPECT_STREQ("SEARCH_DIR(\"=" REAL_PREFIX "/lib\");", (const char *)after);

    free(after);
    fixture_teardown(&f);
}

MOLTEST(conda_refuses_to_grow_a_binary) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    unsigned char before[128];
    memset(before, 0, sizeof before);
    memcpy(before, "/short", sizeof "/short");
    ASSERT_TRUE(fs_write_bytes(f.file, before, sizeof before));

    /* A replacement longer than what it replaces cannot be written without
       moving everything after it, and moving it is what would break the file.
       Refusing is the only honest answer. */
    EXPECT_FALSE(conda_rewrite_prefix(f.file, "/short",
                                      "/a/considerably/longer/prefix", true));

    fixture_teardown(&f);
}

MOLTEST(conda_rewrites_a_binary_holding_nul_bytes_throughout) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    /* The reason this cannot be done with string functions: the placeholder
       sits after a NUL, where strstr would never reach. */
    unsigned char before[256];
    memset(before, 0, sizeof before);
    memcpy(before + 100, PLACEHOLDER, sizeof PLACEHOLDER);
    ASSERT_TRUE(fs_write_bytes(f.file, before, sizeof before));

    ASSERT_TRUE(conda_rewrite_prefix(f.file, PLACEHOLDER, REAL_PREFIX, true));

    size_t size = 0;
    unsigned char *after = fs_read_bytes(f.file, &size);
    ASSERT_TRUE(after != NULL);
    EXPECT_EQ((int)sizeof before, (int)size);
    EXPECT_TRUE(memcmp(after + 100, REAL_PREFIX, strlen(REAL_PREFIX)) == 0);
    free(after);

    fixture_teardown(&f);
}

MOLTEST(conda_rewrites_every_file_the_metadata_names) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    char script[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(script, sizeof script, "%s/activate.sh", f.root));
    ASSERT_TRUE(fs_write_file(script, "PREFIX=" PLACEHOLDER "\n"));

    /* A file the metadata lists and the extraction did not produce: Pickup
       chooses what to unpack, so this is normal and must not fail the whole
       installation. */
    static const char paths_json[] =
        "{\"paths_version\": 1, \"paths\": ["
        "  {\"_path\": \"activate.sh\", \"path_type\": \"hardlink\","
        "   \"prefix_placeholder\": \"" PLACEHOLDER "\", \"file_mode\": \"text\"},"
        "  {\"_path\": \"never/unpacked\", \"path_type\": \"hardlink\","
        "   \"prefix_placeholder\": \"" PLACEHOLDER "\", \"file_mode\": \"text\"},"
        "  {\"_path\": \"plain.txt\", \"path_type\": \"hardlink\"}"
        "]}";

    size_t rewritten = 0;
    ASSERT_TRUE(conda_rewrite_all(paths_json, f.root, &rewritten));
    EXPECT_EQ(1, (int)rewritten);

    char *after = fs_read_file(script);
    ASSERT_TRUE(after != NULL);
    EXPECT_TRUE(strstr(after, f.root) != NULL);
    EXPECT_TRUE(strstr(after, PLACEHOLDER) == NULL);
    free(after);

    fixture_teardown(&f);
}

MOLTEST(conda_leaves_alone_what_carries_no_prefix) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    ASSERT_TRUE(fs_write_file(f.file, "nothing to see here\n"));
    static const char paths_json[] =
        "{\"paths\": [{\"_path\": \"subject\", \"path_type\": \"hardlink\"}]}";

    /* Most files record nothing, and reading every one of them looking for a
       prefix it never had would cost a pass over the whole installation. */
    size_t rewritten = 0;
    ASSERT_TRUE(conda_rewrite_all(paths_json, f.root, &rewritten));
    EXPECT_EQ(0, (int)rewritten);

    fixture_teardown(&f);
}

MOLTEST(conda_refuses_metadata_that_is_not_json) {
    size_t rewritten = 0;
    EXPECT_FALSE(conda_rewrite_all("<html>error</html>", "/tmp", &rewritten));
}

/* --- reading the container --- */

/* Write a zip holding one stored member, the way a .conda is laid out. */
static bool write_zip(const char *path, const char *name, const char *contents) {
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;

    size_t name_length = strlen(name);
    size_t size = strlen(contents);
    unsigned char header[30] = { 'P', 'K', 3, 4 };
    header[8] = 0;  /* stored */
    header[18] = (unsigned char)(size & 0xFF);
    header[19] = (unsigned char)((size >> 8) & 0xFF);
    header[22] = header[18];
    header[23] = header[19];
    header[26] = (unsigned char)(name_length & 0xFF);
    header[27] = (unsigned char)((name_length >> 8) & 0xFF);

    bool ok = fwrite(header, 1, sizeof header, file) == sizeof header
        && fwrite(name, 1, name_length, file) == name_length
        && fwrite(contents, 1, size, file) == size;
    return fclose(file) == 0 && ok;
}

MOLTEST(zip_finds_a_member_by_what_its_name_begins_with) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    char archive[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(archive, sizeof archive, "%s/package.conda", f.root));
    /* Matched by prefix because the rest is the package name and version,
       which change with every build. */
    ASSERT_TRUE(write_zip(archive, "pkg-gcc-16.1.0-h5.tar.zst", "payload bytes"));

    zip_member member;
    ASSERT_TRUE(zip_find_member(archive, "pkg-", &member));
    EXPECT_STREQ("pkg-gcc-16.1.0-h5.tar.zst", member.name);
    EXPECT_TRUE(member.size == (long long)strlen("payload bytes"));

    char extracted[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(extracted, sizeof extracted, "%s/out", f.root));
    ASSERT_TRUE(zip_extract_member(archive, &member, extracted));

    char *contents = fs_read_file(extracted);
    ASSERT_TRUE(contents != NULL);
    EXPECT_STREQ("payload bytes", contents);
    free(contents);

    fixture_teardown(&f);
}

MOLTEST(zip_finds_nothing_that_is_not_there) {
    fixture f;
    ASSERT_TRUE(fixture_setup(&f));

    char archive[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(archive, sizeof archive, "%s/package.conda", f.root));
    ASSERT_TRUE(write_zip(archive, "info-gcc-16.1.0-h5.tar.zst", "metadata"));

    zip_member member;
    EXPECT_FALSE(zip_find_member(archive, "pkg-", &member));
    /* And a file that is no zip at all is refused rather than misread. */
    EXPECT_FALSE(zip_find_member(f.file, "pkg-", &member));
    EXPECT_FALSE(zip_find_member("/nonexistent/pickup.conda", "pkg-", &member));

    fixture_teardown(&f);
}
