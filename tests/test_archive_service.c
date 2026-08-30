#include <moltest.h>

#include <pickup/services/archive_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/process_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char root[64];
} archive_fixture;

static bool fixture_setup(archive_fixture *fixture) {
    return moltest_temp_dir("pickup_tar", fixture->root, sizeof fixture->root);
}

static void fixture_teardown(archive_fixture *fixture) {
    (void)fs_remove_tree(fixture->root);
}

/*
 * Pack `member` from `directory` into `archive`, without a shell.
 *
 * It used to be `system("tar -czf ...")`, and that was two bugs in one line.
 * A shell splits on spaces and eats backslashes, so a Windows path arrived at
 * tar in pieces; and tar reads a name whose first colon comes before any slash
 * as `host:path`, so `C:\Users\...` was a machine called `C` rather than a
 * file. An argv fixes the first — nothing parses it — and --force-local the
 * second, asked for the way the production code asks.
 */
static bool pack(const char *directory, const char *member, const char *archive,
                 const char *compression) {
    const char *argv[8];
    size_t count = 0;
    argv[count++] = "tar";
    if (archive_supports_force_local())
        argv[count++] = "--force-local";
    argv[count++] = "-C";
    argv[count++] = directory;
    argv[count++] = compression;
    argv[count++] = archive;
    argv[count++] = member;
    argv[count] = NULL;

    const process_result result = process_try(argv, NULL);
    return result.completed && result.exit_code == 0;
}

/* Build an archive shaped like the ones LLVM publishes: everything under one
   top-level directory named after the release. */
static bool make_archive(const char *root, const char *archive) {
    char inner[256];
    snprintf(inner, sizeof inner, "%s/LLVM-1.0.0-Linux-X64/bin", root);
    if (!fs_make_dirs(inner))
        return false;

    char binary[512];
    snprintf(binary, sizeof binary, "%s/clang", inner);
    if (!fs_write_file(binary, "#!/bin/sh\n"))
        return false;

    return pack(root, "LLVM-1.0.0-Linux-X64", archive, "-czf");
}

MOLTEST(archive_reports_whether_it_can_extract) {
    bool first = archive_available();
    EXPECT_TRUE(archive_available() == first);
    EXPECT_STREQ("tar", archive_requirement());
}

MOLTEST(archive_settles_the_force_local_question_by_asking) {
    if (!archive_available())
        SKIP("tar is not installed");

    /* Same contract as the wildcards question: asked once, and the answer must
       not move under a caller who has already composed a command with it. */
    bool first = archive_supports_force_local();
    EXPECT_TRUE(archive_supports_force_local() == first);
    EXPECT_TRUE(archive_supports_force_local() == first);
}

MOLTEST(archive_settles_the_wildcards_question_by_asking) {
    if (!archive_available())
        SKIP("tar is not installed");

    /* Whatever this tar answers, it must answer the same every time: the
       result is worked out once and reused for every extraction after. */
    bool first = archive_supports_wildcards();
    EXPECT_TRUE(archive_supports_wildcards() == first);
    EXPECT_TRUE(archive_supports_wildcards() == first);
}

MOLTEST(archive_strips_the_top_level_directory) {
    if (!archive_available())
        SKIP("tar is not installed");

    archive_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char archive[256], destination[256];
    snprintf(archive, sizeof archive, "%s/toolchain.tar.gz", fixture.root);
    snprintf(destination, sizeof destination, "%s/installed", fixture.root);
    ASSERT_TRUE(make_archive(fixture.root, archive));
    ASSERT_TRUE(fs_make_dirs(destination));

    ASSERT_TRUE(archive_extract(archive, destination, 1));

    /* bin/clang must land directly in the destination, because that is where
       the scanner will look for it. */
    char installed[512];
    snprintf(installed, sizeof installed, "%s/bin/clang", destination);
    EXPECT_TRUE(fs_path_exists(installed));

    /* And not one level down, under the archive's own directory name. */
    char nested[512];
    snprintf(nested, sizeof nested, "%s/LLVM-1.0.0-Linux-X64", destination);
    EXPECT_FALSE(fs_path_exists(nested));

    fixture_teardown(&fixture);
}

MOLTEST(archive_keeps_the_top_level_directory_when_not_stripping) {
    if (!archive_available())
        SKIP("tar is not installed");

    archive_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char archive[256], destination[256];
    snprintf(archive, sizeof archive, "%s/toolchain.tar.gz", fixture.root);
    snprintf(destination, sizeof destination, "%s/raw", fixture.root);
    ASSERT_TRUE(make_archive(fixture.root, archive));
    ASSERT_TRUE(fs_make_dirs(destination));

    ASSERT_TRUE(archive_extract(archive, destination, 0));

    char nested[512];
    snprintf(nested, sizeof nested, "%s/LLVM-1.0.0-Linux-X64/bin/clang", destination);
    EXPECT_TRUE(fs_path_exists(nested));

    fixture_teardown(&fixture);
}

/* An archive shaped like a release: a little of what is wanted, a lot of what
   is not. */
static bool make_mixed_archive(const char *root, const char *archive) {
    const char *files[] = {
        "LLVM-1.0.0/bin/clang",
        "LLVM-1.0.0/bin/mlir-opt",
        "LLVM-1.0.0/bin/flang-1",
        "LLVM-1.0.0/lib/clang/1/include/stddef.h",
        "LLVM-1.0.0/lib/clang/1/lib/libflang_rt.runtime.a",
        "LLVM-1.0.0/lib/libLLVMCore.a",
        "LLVM-1.0.0/share/doc/readme",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        char *slash = strrchr(path, '/');
        *slash = '\0';
        if (!fs_make_dirs(path))
            return false;
        *slash = '/';
        if (!fs_write_file(path, "x"))
            return false;
    }

    return pack(root, "LLVM-1.0.0", archive, "-czf");
}

MOLTEST(archive_extracts_only_what_was_asked_for) {
    if (!archive_available())
        SKIP("tar is not installed");

    archive_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char archive[256], destination[256];
    snprintf(archive, sizeof archive, "%s/release.tar.gz", fixture.root);
    snprintf(destination, sizeof destination, "%s/installed", fixture.root);
    ASSERT_TRUE(make_mixed_archive(fixture.root, archive));
    ASSERT_TRUE(fs_make_dirs(destination));

    const char *const patterns[] = { "*/bin/clang", "*/lib/clang/*" };
    const char *const excludes[] = { "*flang*" };
    const archive_request request = {
        .patterns = patterns, .pattern_count = 2,
        .excludes = excludes, .exclude_count = 1,
        .strip_components = 1,
    };
    ASSERT_TRUE(archive_extract_selected(archive, destination, &request));

    char path[512];
    snprintf(path, sizeof path, "%s/bin/clang", destination);
    EXPECT_TRUE(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/lib/clang/1/include/stddef.h", destination);
    EXPECT_TRUE(fs_path_exists(path));

    /* The rest of the release is never written, not written and deleted: on a
       real one that is the difference between needing 11 GB free and half of
       one. */
    snprintf(path, sizeof path, "%s/bin/mlir-opt", destination);
    EXPECT_FALSE(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/lib/libLLVMCore.a", destination);
    EXPECT_FALSE(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/share/doc/readme", destination);
    EXPECT_FALSE(fs_path_exists(path));

    /* Excluded even though the pattern matched it. */
    snprintf(path, sizeof path, "%s/lib/clang/1/lib/libflang_rt.runtime.a", destination);
    EXPECT_FALSE(fs_path_exists(path));

    fixture_teardown(&fixture);
}

MOLTEST(archive_selects_by_pattern_whichever_tar_this_is) {
    if (!archive_available())
        SKIP("tar is not installed");

    archive_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char archive[256], destination[256];
    snprintf(archive, sizeof archive, "%s/release.tar.gz", fixture.root);
    snprintf(destination, sizeof destination, "%s/picked", fixture.root);
    ASSERT_TRUE(make_mixed_archive(fixture.root, archive));
    ASSERT_TRUE(fs_make_dirs(destination));

    const char *const patterns[] = { "*/bin/clang" };
    const archive_request request = {
        .patterns = patterns, .pattern_count = 1,
        .strip_components = 1,
    };
    ASSERT_TRUE(archive_extract_selected(archive, destination, &request));

    /* The point of detecting the flag: a glob has to select by pattern on
       either implementation. GNU tar without --wildcards would read this as a
       literal file name and match nothing; bsdtar handed --wildcards would
       refuse the option outright. Both come out the same way here — an empty
       destination — so a passing selection is what says the detection was
       right. */
    char path[512];
    snprintf(path, sizeof path, "%s/bin/clang", destination);
    EXPECT_TRUE(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/bin/mlir-opt", destination);
    EXPECT_FALSE(fs_path_exists(path));

    fixture_teardown(&fixture);
}

MOLTEST(archive_without_patterns_extracts_everything) {
    if (!archive_available())
        SKIP("tar is not installed");

    archive_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char archive[256], destination[256];
    snprintf(archive, sizeof archive, "%s/release.tar.gz", fixture.root);
    snprintf(destination, sizeof destination, "%s/full", fixture.root);
    ASSERT_TRUE(make_mixed_archive(fixture.root, archive));
    ASSERT_TRUE(fs_make_dirs(destination));

    const archive_request request = { .strip_components = 1 };
    ASSERT_TRUE(archive_extract_selected(archive, destination, &request));

    char path[512];
    snprintf(path, sizeof path, "%s/bin/mlir-opt", destination);
    EXPECT_TRUE(fs_path_exists(path));
    snprintf(path, sizeof path, "%s/share/doc/readme", destination);
    EXPECT_TRUE(fs_path_exists(path));

    fixture_teardown(&fixture);
}

MOLTEST(archive_extracts_at_the_most_it_will_accept) {
    if (!archive_available())
        SKIP("tar is not installed");

    archive_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char archive[256], destination[256];
    snprintf(archive, sizeof archive, "%s/release.tar.gz", fixture.root);
    snprintf(destination, sizeof destination, "%s/loaded", fixture.root);
    ASSERT_TRUE(make_mixed_archive(fixture.root, archive));
    ASSERT_TRUE(fs_make_dirs(destination));

    /* The most `build_command` accepts, on both axes at once: 24 and 24 is
       inside the limit, so it is a command Pickup will really compose. Only
       the first pattern selects anything; the rest are there to fill the argv.

       What this guards is the end of that argv. `push` stops one short of the
       array, and the entry it refuses at full load is the terminating NULL —
       leaving execve to read the end of the list out of whatever follows in
       memory. Nothing about the composed command looks wrong, and the failure
       has no relationship to the option that overflowed it. */
    const char *patterns[ARCHIVE_MAX_PATTERNS];
    const char *excludes[ARCHIVE_MAX_PATTERNS];
    patterns[0] = "*/bin/clang";
    for (size_t i = 1; i < ARCHIVE_MAX_PATTERNS; i++)
        patterns[i] = "*/nothing/matches/this";
    for (size_t i = 0; i < ARCHIVE_MAX_PATTERNS; i++)
        excludes[i] = "*never-here*";

    const archive_request request = {
        .patterns = patterns, .pattern_count = ARCHIVE_MAX_PATTERNS,
        .excludes = excludes, .exclude_count = ARCHIVE_MAX_PATTERNS,
        .strip_components = 1,
    };
    ASSERT_TRUE(archive_extract_selected(archive, destination, &request));

    char path[512];
    snprintf(path, sizeof path, "%s/bin/clang", destination);
    EXPECT_TRUE(fs_path_exists(path));

    fixture_teardown(&fixture);
}

MOLTEST(archive_refuses_more_patterns_than_it_can_hold) {
    const char *many[ARCHIVE_MAX_PATTERNS + 1];
    for (size_t i = 0; i < ARCHIVE_MAX_PATTERNS + 1; i++)
        many[i] = "*";

    const archive_request request = {
        .patterns = many, .pattern_count = ARCHIVE_MAX_PATTERNS + 1,
        .strip_components = 1,
    };
    /* Refused rather than silently truncated: extracting a subset of the
       subset asked for would install a toolchain missing pieces. */
    EXPECT_FALSE(archive_extract_selected("/nonexistent.tar.gz", "/tmp", &request));
}

MOLTEST(archive_fails_on_something_that_is_not_an_archive) {
    if (!archive_available())
        SKIP("tar is not installed");

    archive_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char rubbish[256], destination[256];
    snprintf(rubbish, sizeof rubbish, "%s/not-an-archive.tar.xz", fixture.root);
    snprintf(destination, sizeof destination, "%s/out", fixture.root);
    ASSERT_TRUE(fs_write_file(rubbish, "404: Not Found\n"));
    ASSERT_TRUE(fs_make_dirs(destination));

    /* An error page saved under an archive's name must be reported, not
       silently treated as an empty toolchain. */
    EXPECT_FALSE(archive_extract(rubbish, destination, 1));
    EXPECT_FALSE(archive_extract("/nonexistent/archive.tar.xz", destination, 1));

    fixture_teardown(&fixture);
}

MOLTEST(archive_opens_what_the_registry_packs) {
    if (!archive_available())
        SKIP("tar is not installed");

    /* The registry packs everything as tar.zst, so this is the difference
       between being able to install and not. The probe opens an archive rather
       than reading a version string, which is why it is worth asserting at all:
       a tar that merely accepts --zstd would still pass a weaker check. */
    if (!archive_supports_zstd())
        SKIP("this tar cannot open zstd archives");

    EXPECT_TRUE(archive_supports_zstd());
    EXPECT_STREQ("zstd", archive_zstd_requirement());
}

MOLTEST(archive_extracts_a_zstd_archive_without_a_top_level_directory) {
    if (!archive_available() || !archive_supports_zstd())
        SKIP("tar with zstd is needed to unpack what the registry publishes");

    char root[PICKUP_PATHS_MAX];
    ASSERT_TRUE(moltest_temp_dir("pickup_zstd_extract", root, sizeof root));

    char stage[PICKUP_PATHS_MAX];
    char archive[PICKUP_PATHS_MAX];
    char destination[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(stage, sizeof stage, "%s/stage/bin", root));
    ASSERT_TRUE(fs_make_dirs(stage));

    char file[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/thing", stage));
    ASSERT_TRUE(fs_write_file(file, "#!/bin/sh\n"));

    ASSERT_TRUE(fs_format_path(archive, sizeof archive, "%s/thing.tar.zst", root));

    char staged[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(staged, sizeof staged, "%s/stage", root));
    ASSERT_TRUE(pack(staged, ".", archive, "-caf"));

    /* No leading component to strip: the registry publishes archives whose bin
       and lib are already at the top. */
    ASSERT_TRUE(fs_format_path(destination, sizeof destination, "%s/out", root));
    ASSERT_TRUE(fs_make_dirs(destination));
    ASSERT_TRUE(archive_extract(archive, destination, 0));

    char landed[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(landed, sizeof landed, "%s/bin/thing", destination));
    EXPECT_TRUE(fs_path_exists(landed));

    (void)fs_remove_tree(root);
}
