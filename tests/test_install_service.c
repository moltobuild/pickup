#include <moltest.h>

#include <pickup/services/archive_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/install_service.h>
#include <pickup/services/process_service.h>
#include <pickup/util/sha256.h>

#include "user_dirs_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* An install is exercised end to end over file://, so the whole path runs —
   download, digest, unpack, prove, rename — without a network or a gigabyte.
   The compiler inside the archive is a wrapper around the real one, because the
   last step asks it to identify itself and nothing else would. */
typedef struct {
    char root[64];
    char previous_home[4096];
    bool had_home;
} install_fixture;

static bool fixture_setup(install_fixture *fixture) {
    if (!moltest_temp_dir("pickup_install", fixture->root, sizeof fixture->root))
        return false;

    const char *existing = getenv(PICKUP_HOME_ENV);
    fixture->had_home = existing != NULL;
    if (existing != NULL)
        snprintf(fixture->previous_home, sizeof fixture->previous_home, "%s", existing);

    char home[128];
    snprintf(home, sizeof home, "%s/home", fixture->root);
    return setenv(PICKUP_HOME_ENV, home, 1) == 0;
}

static void fixture_teardown(install_fixture *fixture) {
    if (fixture->had_home)
        (void)setenv(PICKUP_HOME_ENV, fixture->previous_home, 1);
    else
        (void)unsetenv(PICKUP_HOME_ENV);
    (void)fs_remove_tree(fixture->root);
}

static bool tools_present(void) {
    return http_available() && archive_available() && archive_supports_zstd()
        && access("/usr/bin/gcc-12", X_OK) == 0;
}

/*
 * Pack `stage` the way the registry packs everything: zstd, and with bin and
 * lib already at the top rather than under a directory named after the release.
 */
/*
 * Pack everything under `stage` into `archive`, without a shell.
 *
 * The same two bugs test_archive_service.c records, in one line here too: a
 * shell splits on spaces and eats backslashes, so a Windows path reached tar
 * in pieces; and tar reads a name whose first colon comes before any slash as
 * `host:path`, so `D:\a\...` was a machine called `D`. An argv fixes the
 * first — nothing parses it — and --force-local the second, asked for the way
 * the production code asks.
 */
static bool pack(const char *stage, const char *archive) {
    const char *argv[8];
    size_t count = 0;
    argv[count++] = "tar";
    if (archive_supports_force_local())
        argv[count++] = "--force-local";
    argv[count++] = "-C";
    argv[count++] = stage;
    argv[count++] = "-caf";
    argv[count++] = archive;
    argv[count++] = ".";
    argv[count] = NULL;

    const process_result result = process_try(argv, NULL);
    return result.completed && result.exit_code == 0;
}

/* Describe an archive the way the registry describes it. */
static bool describe(const char *archive, const char *name, const char *version,
                     registry_kind kind, registry_artifact *out) {
    *out = (registry_artifact){ 0 };
    out->kind = kind;
    snprintf(out->name, sizeof out->name, "%s", name);
    snprintf(out->version, sizeof out->version, "%s", version);
    snprintf(out->target, sizeof out->target, "%s", "linux-x86_64");
    snprintf(out->format, sizeof out->format, "%s", REGISTRY_FORMAT_TAR_ZST);
    snprintf(out->download_url, sizeof out->download_url, "file://%s", archive);

    long long size = 0;
    if (!fs_file_size(archive, &size))
        return false;
    out->size_bytes = size;
    return sha256_file(archive, out->checksum);
}

/* An archive holding a working compiler at bin/clang. */
static bool make_toolchain(install_fixture *fixture, registry_artifact *artifact) {
    char bin[256];
    snprintf(bin, sizeof bin, "%s/stage/bin", fixture->root);
    if (!fs_make_dirs(bin))
        return false;

    char driver[512];
    snprintf(driver, sizeof driver, "%s/clang", bin);
    /* A compiler that really compiles: what is installed here is probed
       afterwards, so imitating one would not be enough. Whichever gcc the
       platform has, rather than a pinned one — the test needs a working
       driver, not a particular version. */
    if (!moltest_fake_program(driver, "exec gcc\n", NULL, 0))
        return false;

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/clang.tar.zst", fixture->root);
    snprintf(stage, sizeof stage, "%s/stage", fixture->root);
    if (!pack(stage, archive))
        return false;
    return describe(archive, "clang", "1.0.0", registry_kind_toolchain, artifact);
}

MOLTEST(install_places_a_verified_toolchain_under_the_pickup_home) {
    if (!tools_present())
        SKIP("curl, tar with zstd and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_toolchain(&fixture, &artifact));

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);

    ASSERT_EQ(install_ok, report.status);
    EXPECT_TRUE(install_succeeded(report.status));

    /* Installed under the pickup home, never anywhere needing privileges. */
    char home[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_home(home, sizeof home));
    EXPECT_TRUE(strncmp(report.directory, home, strlen(home)) == 0);

    /* Unpacked with nothing stripped: what the registry packs is already at the
       top, and dropping a component would put bin one level too high. */
    char driver[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(driver, sizeof driver, "%s/bin/clang", report.directory));
    EXPECT_TRUE(fs_path_exists(driver));

    /* The directory is named from what the compiler said it is, not from what
       the registry called the artifact. */
    EXPECT_TRUE(strstr(report.directory, "1.0.0") == NULL);
    EXPECT_TRUE(report.installed.version.major > 0);
    EXPECT_TRUE(strlen(report.installed.target) > 0);
    EXPECT_TRUE(report.features_proven > 0);
    EXPECT_TRUE(report.installed_size > 0);

    /* Nothing half-finished is left behind. */
    char partial[PICKUP_PATHS_MAX];
    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));
    ASSERT_TRUE(fs_format_path(partial, sizeof partial, "%s/.partial", toolchains));
    EXPECT_FALSE(fs_path_exists(partial));

    fixture_teardown(&fixture);
}

MOLTEST(install_discards_an_archive_whose_digest_does_not_match) {
    if (!tools_present())
        SKIP("curl, tar with zstd and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_toolchain(&fixture, &artifact));
    /* One byte of the expected digest changed: a tampered or truncated
       download must never be unpacked. */
    artifact.checksum[0] = artifact.checksum[0] == 'a' ? 'b' : 'a';

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);

    EXPECT_EQ(install_hash_mismatch, report.status);
    EXPECT_FALSE(install_succeeded(report.status));
    EXPECT_TRUE(strlen(report.expected) > 0);
    EXPECT_TRUE(strlen(report.actual) > 0);
    EXPECT_FALSE(sha256_hex_equal(report.expected, report.actual));

    /* And nothing is installed as a result of it. */
    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));
    char partial[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(partial, sizeof partial, "%s/.partial", toolchains));
    EXPECT_FALSE(fs_path_exists(partial));

    fixture_teardown(&fixture);
}

MOLTEST(install_refuses_what_was_withdrawn_unless_told_otherwise) {
    if (!tools_present())
        SKIP("curl, tar with zstd and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_toolchain(&fixture, &artifact));
    artifact.yanked = true;

    const install_request refused = { .artifact = &artifact, .allow_yanked = false };
    install_report report = install_run(&refused);
    EXPECT_EQ(install_yanked, report.status);

    /* Refused before spending the download, not after. */
    char downloads[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    EXPECT_FALSE(fs_path_exists(downloads));

    /* Withdrawn is not gone: naming it is allowed, and then it installs. */
    const install_request named = { .artifact = &artifact, .allow_yanked = true };
    report = install_run(&named);
    EXPECT_EQ(install_ok, report.status);

    fixture_teardown(&fixture);
}

MOLTEST(install_refuses_a_packing_it_cannot_open) {
    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact = { 0 };
    snprintf(artifact.name, sizeof artifact.name, "%s", "clang");
    snprintf(artifact.version, sizeof artifact.version, "%s", "1.0.0");
    snprintf(artifact.format, sizeof artifact.format, "%s", "tar.br");
    snprintf(artifact.download_url, sizeof artifact.download_url, "%s",
             "file:///nowhere");

    /* How a blob is packed is something the registry states, and this build
       opens three packings, of which brotli is not one. Refused on the
       statement, before anything is fetched and without inferring anything
       from the URL.

       Not to be confused with a packing this build knows and this tar cannot
       open: that one is install_no_codec, and it names a program to install.
       This is the registry saying something pickup has never heard of. */
    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);
    EXPECT_EQ(install_unsupported_format, report.status);

    char downloads[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    EXPECT_FALSE(fs_path_exists(downloads));

    fixture_teardown(&fixture);
}

/* The packing this machine's tar can actually open, or NULL if it opens none.
   Asked rather than assumed -- assuming was the bug these tests exist for. */
static const char *a_packing_this_tar_opens(void) {
    if (archive_supports_gzip())
        return REGISTRY_FORMAT_TAR_GZ;
    if (archive_supports_xz())
        return REGISTRY_FORMAT_TAR_XZ;
    if (archive_supports_zstd())
        return REGISTRY_FORMAT_TAR_ZST;
    return NULL;
}

/* And one it cannot, or NULL if it opens all of them. */
static const char *a_packing_this_tar_refuses(void) {
    if (!archive_supports_zstd())
        return REGISTRY_FORMAT_TAR_ZST;
    if (!archive_supports_xz())
        return REGISTRY_FORMAT_TAR_XZ;
    if (!archive_supports_gzip())
        return REGISTRY_FORMAT_TAR_GZ;
    return NULL;
}

MOLTEST(install_lets_through_a_packing_this_tar_opens) {
    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    const char *packing = a_packing_this_tar_opens();
    if (packing == NULL) {
        /* A tar that opens nothing has no question to answer here. */
        fixture_teardown(&fixture);
        return;
    }

    registry_artifact artifact = { 0 };
    snprintf(artifact.name, sizeof artifact.name, "%s", "clang");
    snprintf(artifact.version, sizeof artifact.version, "%s", "1.0.0");
    snprintf(artifact.format, sizeof artifact.format, "%s", packing);
    snprintf(artifact.download_url, sizeof artifact.download_url, "%s", "file:///nowhere");

    /* The URL is nowhere, so the download is what fails -- and that it got as
       far as the download is the assertion: the packing was not the reason. */
    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);
    EXPECT_TRUE(report.status != install_unsupported_format);
    EXPECT_TRUE(report.status != install_no_codec);

    fixture_teardown(&fixture);
}

MOLTEST(install_refuses_a_packing_this_tar_cannot_open_before_downloading) {
    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    const char *packing = a_packing_this_tar_refuses();
    if (packing == NULL) {
        /* A tar that opens all three: nothing to be turned away. */
        fixture_teardown(&fixture);
        return;
    }

    registry_artifact artifact = { 0 };
    snprintf(artifact.name, sizeof artifact.name, "%s", "clang");
    snprintf(artifact.version, sizeof artifact.version, "%s", "1.0.0");
    snprintf(artifact.format, sizeof artifact.format, "%s", packing);
    snprintf(artifact.download_url, sizeof artifact.download_url, "%s", "file:///nowhere");

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);
    EXPECT_EQ(install_no_codec, report.status);

    /* Before the transfer, not after it. The whole point of asking early is
       that the answer costs nothing: an artifact refused here has not been
       downloaded, so the downloads directory was never even made. */
    char downloads[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    EXPECT_FALSE(fs_path_exists(downloads));

    fixture_teardown(&fixture);
}

MOLTEST(install_names_the_program_a_packing_needs) {
    /* Every packing pickup accepts has a program to name when the tar cannot
       open it; anything else is not a packing pickup knows. */
    EXPECT_TRUE(install_format_requirement(REGISTRY_FORMAT_TAR_GZ) != NULL);
    EXPECT_TRUE(install_format_requirement(REGISTRY_FORMAT_TAR_XZ) != NULL);
    EXPECT_TRUE(install_format_requirement(REGISTRY_FORMAT_TAR_ZST) != NULL);
    EXPECT_TRUE(install_format_requirement("tar.br") == NULL);
    EXPECT_TRUE(install_format_requirement(NULL) == NULL);
}

MOLTEST(install_rejects_an_archive_without_a_compiler_in_it) {
    if (!tools_present())
        SKIP("curl, tar with zstd and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    /* An archive that unpacks fine but holds nothing that identifies itself. */
    char share[256];
    snprintf(share, sizeof share, "%s/stage/share", fixture.root);
    ASSERT_TRUE(fs_make_dirs(share));
    char readme[512];
    snprintf(readme, sizeof readme, "%s/README", share);
    ASSERT_TRUE(fs_write_file(readme, "not a compiler\n"));

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/empty.tar.zst", fixture.root);
    snprintf(stage, sizeof stage, "%s/stage", fixture.root);
    ASSERT_TRUE(pack(stage, archive));

    registry_artifact artifact;
    ASSERT_TRUE(describe(archive, "clang", "1.0.0", registry_kind_toolchain, &artifact));

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);

    EXPECT_EQ(install_not_a_toolchain, report.status);

    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));
    char partial[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(partial, sizeof partial, "%s/.partial", toolchains));
    EXPECT_FALSE(fs_path_exists(partial));

    fixture_teardown(&fixture);
}

MOLTEST(install_puts_a_tool_where_nothing_resolves_against_it) {
    if (!http_available() || !archive_available() || !archive_supports_zstd())
        SKIP("curl and tar with zstd are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char bin[256];
    snprintf(bin, sizeof bin, "%s/stage/bin", fixture.root);
    ASSERT_TRUE(fs_make_dirs(bin));

    /* Where it landed, not where it was asked for: the platform decides the
       filename, and a recipe published for this platform names the file that
       is actually in the archive. Declaring `bin/clang-format` over a
       `bin/clang-format.exe` is a recipe for another platform, and install is
       right to answer that nothing is there. */
    char binary[512];
    snprintf(binary, sizeof binary, "%s/clang-format", bin);
    ASSERT_TRUE(moltest_fake_program(binary, "out clang-format version 1.0.0\nexit 0\n", binary,
                                     sizeof binary));

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/cf.tar.zst", fixture.root);
    snprintf(stage, sizeof stage, "%s/stage", fixture.root);
    ASSERT_TRUE(pack(stage, archive));

    const char *slash = strrchr(binary, '/');
    ASSERT_TRUE(slash != NULL);

    registry_artifact artifact;
    ASSERT_TRUE(describe(archive, "clang-format", "1.0.0", registry_kind_tool, &artifact));
    snprintf(artifact.binary, sizeof artifact.binary, "bin/%s", slash + 1);

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);
    ASSERT_EQ(install_ok, report.status);

    /* Kept apart from the toolchains, because nothing resolves against it, and
       named after the version so two of them do not collide. */
    char tools[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_tools(tools, sizeof tools));
    EXPECT_TRUE(strncmp(report.directory, tools, strlen(tools)) == 0);
    EXPECT_TRUE(strstr(report.directory, "clang-format-1.0.0") != NULL);

    fixture_teardown(&fixture);
}

/* With nothing relocated, the tool lands in the data directory and the archive
   in the cache directory: the platform's places, and not the legacy home. */
MOLTEST(install_uses_the_platform_directories_when_nothing_is_relocated) {
    if (!http_available() || !archive_available() || !archive_supports_zstd())
        SKIP("curl and tar with zstd are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    user_dirs_fixture dirs;
    ASSERT_TRUE(user_dirs_setup(&dirs, "pickup_install_dirs"));

    char bin[256];
    snprintf(bin, sizeof bin, "%s/stage/bin", fixture.root);
    ASSERT_TRUE(fs_make_dirs(bin));
    char binary[512];
    snprintf(binary, sizeof binary, "%s/clang-format", bin);
    ASSERT_TRUE(moltest_fake_program(binary, "out clang-format version 1.0.0\nexit 0\n", binary,
                                     sizeof binary));

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/cf.tar.zst", fixture.root);
    snprintf(stage, sizeof stage, "%s/stage", fixture.root);
    ASSERT_TRUE(pack(stage, archive));

    const char *slash = strrchr(binary, '/');
    ASSERT_TRUE(slash != NULL);
    registry_artifact artifact;
    ASSERT_TRUE(describe(archive, "clang-format", "1.0.0", registry_kind_tool, &artifact));
    snprintf(artifact.binary, sizeof artifact.binary, "bin/%s", slash + 1);

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);
    ASSERT_EQ(install_ok, report.status);

    char data[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_data(data, sizeof data));
    EXPECT_TRUE(strncmp(report.directory, data, strlen(data)) == 0);

    char downloads[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    EXPECT_TRUE(fs_is_dir(downloads));
    char cache[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    EXPECT_TRUE(strncmp(downloads, cache, strlen(cache)) == 0);

    char legacy[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_legacy_home(legacy, sizeof legacy));
    EXPECT_FALSE(fs_path_exists(legacy));

    user_dirs_teardown(&dirs);
    fixture_teardown(&fixture);
}

MOLTEST(install_rejects_a_tool_whose_binary_says_nothing) {
    if (!http_available() || !archive_available() || !archive_supports_zstd())
        SKIP("curl and tar with zstd are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char bin[256];
    snprintf(bin, sizeof bin, "%s/stage/bin", fixture.root);
    ASSERT_TRUE(fs_make_dirs(bin));

    /* Unpacks, and the binary the registry named is not in it. Nothing is
       adopted for having unpacked. */
    char other[512];
    snprintf(other, sizeof other, "%s/something-else", bin);
    ASSERT_TRUE(moltest_fake_program(other, "exit 0\n", NULL, 0));

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/cf.tar.zst", fixture.root);
    snprintf(stage, sizeof stage, "%s/stage", fixture.root);
    ASSERT_TRUE(pack(stage, archive));

    registry_artifact artifact;
    ASSERT_TRUE(describe(archive, "clang-format", "1.0.0", registry_kind_tool, &artifact));
    snprintf(artifact.binary, sizeof artifact.binary, "%s", "bin/clang-format");

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);
    EXPECT_EQ(install_not_a_tool, report.status);

    char tools[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_tools(tools, sizeof tools));
    char partial[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(partial, sizeof partial, "%s/.partial", tools));
    EXPECT_FALSE(fs_path_exists(partial));

    fixture_teardown(&fixture);
}

MOLTEST(install_status_messages_cover_every_outcome) {
    /* A caller prints these; none may come out blank. */
    const install_status all[] = {
        install_ok, install_no_downloader, install_no_extractor, install_no_codec,
        install_unsupported_format, install_yanked, install_download_failed,
        install_hash_mismatch, install_extract_failed, install_extract_stalled,
        install_not_a_toolchain,
        install_not_a_tool, install_path_error,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *message = install_status_message(all[i]);
        EXPECT_TRUE(message != NULL && message[0] != '\0');
    }

    /* One outcome means installed. The registry publishes a digest for
       everything, so there is no such thing as an install that went through
       unverified. */
    EXPECT_TRUE(install_succeeded(install_ok));
    EXPECT_FALSE(install_succeeded(install_hash_mismatch));
    EXPECT_FALSE(install_succeeded(install_yanked));
}

/*
 * An archive holding two drivers: one under a name the search prefers, and the
 * real one under its target triple. llvm-mingw is exactly this shape -- its
 * `bin/clang` emits for the host and only `bin/x86_64-w64-mingw32-clang` emits
 * for Windows -- so picking by convention picks the wrong one.
 */
static bool make_cross_toolchain(install_fixture *fixture, registry_artifact *artifact) {
    char bin[256];
    snprintf(bin, sizeof bin, "%s/stage/bin", fixture->root);
    if (!fs_make_dirs(bin))
        return false;

    char driver[512];
    snprintf(driver, sizeof driver, "%s/cc", bin);
    if (!moltest_fake_program(driver, "exec gcc\n", NULL, 0))
        return false;
    snprintf(driver, sizeof driver, "%s/x86_64-w64-mingw32-gcc", bin);
    if (!moltest_fake_program(driver, "exec gcc\n", NULL, 0))
        return false;

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/cross.tar.zst", fixture->root);
    snprintf(stage, sizeof stage, "%s/stage", fixture->root);
    if (!pack(stage, archive))
        return false;
    if (!describe(archive, "llvm-mingw", "1.0.0", registry_kind_toolchain, artifact))
        return false;

    /* What the recipe published, and what the prefix should be identified
       through. */
    snprintf(artifact->c_driver, sizeof artifact->c_driver, "%s", "bin/x86_64-w64-mingw32-gcc");
    return true;
}

/*
 * The driver a recipe names wins over the one the search would have preferred.
 *
 * Identifying a cross prefix by convention finds the host driver sitting beside
 * the real one, and everything downstream then describes the wrong compiler:
 * the installed directory is named after a target the toolchain does not emit
 * for, and the report names a driver the publisher did not mean.
 */
MOLTEST(install_identifies_a_toolchain_through_the_driver_its_recipe_names) {
    if (!tools_present())
        SKIP("curl, tar with zstd and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_cross_toolchain(&fixture, &artifact));

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);

    ASSERT_EQ(install_ok, report.status);
    EXPECT_STREQ("x86_64-w64-mingw32-gcc", report.installed.name);

    fixture_teardown(&fixture);
}

/* And a recipe that names nothing is still served by the search: the field is
   a hint the publisher may leave out, not a new requirement. */
MOLTEST(install_falls_back_to_the_search_when_a_recipe_names_no_driver) {
    if (!tools_present())
        SKIP("curl, tar with zstd and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_cross_toolchain(&fixture, &artifact));
    artifact.c_driver[0] = '\0';

    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);

    ASSERT_EQ(install_ok, report.status);
    EXPECT_STREQ("cc", report.installed.name);

    fixture_teardown(&fixture);
}

/* --- what is already there --- */

/* Packed as gzip, which every tar opens, so these run on a machine without
   zstd — a Mac as it ships is one. */
static bool gzip_installs_work(void) {
    return http_available() && archive_available() && archive_supports_gzip();
}

/* A clang-format 1.0.0 packed the way the registry packs a tool, as gzip. */
static bool make_gzip_tool(install_fixture *fixture, registry_artifact *artifact) {
    char bin[256];
    snprintf(bin, sizeof bin, "%s/tool-stage/bin", fixture->root);
    if (!fs_make_dirs(bin))
        return false;
    char binary[512];
    snprintf(binary, sizeof binary, "%s/clang-format", bin);
    if (!moltest_fake_program(binary, "out clang-format version 1.0.0\nexit 0\n", binary,
                              sizeof binary))
        return false;

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/cf.tar.gz", fixture->root);
    snprintf(stage, sizeof stage, "%s/tool-stage", fixture->root);
    if (!pack(stage, archive) ||
        !describe(archive, "clang-format", "1.0.0", registry_kind_tool, artifact))
        return false;
    snprintf(artifact->format, sizeof artifact->format, "%s", REGISTRY_FORMAT_TAR_GZ);
    const char *slash = strrchr(binary, '/');
    snprintf(artifact->binary, sizeof artifact->binary, "bin/%s", slash + 1);
    return true;
}

/* The archive a second install would have to download, taken away: an install
   that still succeeds afterwards did not download anything. */
static void take_away_the_download(const registry_artifact *artifact) {
    (void)remove(artifact->download_url + strlen("file://"));
}

/*
 * The second install of the same coordinate downloads nothing.
 *
 * A published coordinate is immutable, so the same name, version and target
 * is the same bytes: fetching them again is a download, a digest and an unpack
 * spent on arriving where things already are.
 */
MOLTEST(install_downloads_nothing_for_a_tool_already_installed) {
    if (!gzip_installs_work())
        SKIP("curl and tar with gzip are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_gzip_tool(&fixture, &artifact));

    const install_request request = { .artifact = &artifact };
    install_report first = install_run(&request);
    ASSERT_EQ(install_ok, first.status);
    EXPECT_FALSE(first.already_installed);

    take_away_the_download(&artifact);

    install_report second = install_run(&request);
    ASSERT_EQ(install_ok, second.status);
    EXPECT_TRUE(second.already_installed);
    EXPECT_STREQ(first.directory, second.directory);

    fixture_teardown(&fixture);
}

/* `--force` is the way to ask for the download anyway: here it is attempted,
   and fails because the archive is gone, which is the proof it was. */
MOLTEST(install_downloads_again_when_forced) {
    if (!gzip_installs_work())
        SKIP("curl and tar with gzip are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_gzip_tool(&fixture, &artifact));
    const install_request request = { .artifact = &artifact };
    ASSERT_EQ(install_ok, install_run(&request).status);

    take_away_the_download(&artifact);

    const install_request forced = { .artifact = &artifact, .force = true };
    install_report report = install_run(&forced);
    EXPECT_EQ(install_download_failed, report.status);
    EXPECT_FALSE(report.already_installed);

    fixture_teardown(&fixture);
}

/* What an install leaves behind says what it installed, which is how the
   next one knows the bytes are the same. */
MOLTEST(install_records_what_it_installed) {
    if (!gzip_installs_work())
        SKIP("curl and tar with gzip are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_gzip_tool(&fixture, &artifact));
    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);
    ASSERT_EQ(install_ok, report.status);

    char receipt[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(receipt, sizeof receipt, "%s/%s", report.directory,
                               INSTALL_RECEIPT_NAME));
    char *text = fs_read_file(receipt);
    ASSERT_TRUE(text != NULL);
    EXPECT_TRUE(strstr(text, artifact.checksum) != NULL);
    EXPECT_TRUE(strstr(text, "clang-format") != NULL);
    EXPECT_TRUE(strstr(text, "1.0.0") != NULL);
    free(text);

    fixture_teardown(&fixture);
}

/* An install that recorded different bytes under the same name is not the
   same thing, however it is named: it is replaced rather than kept. */
MOLTEST(install_replaces_a_tool_that_recorded_other_bytes) {
    if (!gzip_installs_work())
        SKIP("curl and tar with gzip are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_gzip_tool(&fixture, &artifact));
    const install_request request = { .artifact = &artifact };
    install_report first = install_run(&request);
    ASSERT_EQ(install_ok, first.status);

    char receipt[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(receipt, sizeof receipt, "%s/%s", first.directory,
                               INSTALL_RECEIPT_NAME));
    ASSERT_TRUE(fs_write_file(receipt, "checksum = \"0000\"\n"));

    install_report second = install_run(&request);
    ASSERT_EQ(install_ok, second.status);
    EXPECT_FALSE(second.already_installed);

    fixture_teardown(&fixture);
}

/* A tool installed before installs left a record still counts, as long as it
   is where that version is installed and it answers. */
MOLTEST(install_counts_a_tool_installed_before_records_were_kept) {
    if (!gzip_installs_work())
        SKIP("curl and tar with gzip are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    registry_artifact artifact;
    ASSERT_TRUE(make_gzip_tool(&fixture, &artifact));
    const install_request request = { .artifact = &artifact };
    install_report first = install_run(&request);
    ASSERT_EQ(install_ok, first.status);

    char receipt[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(receipt, sizeof receipt, "%s/%s", first.directory,
                               INSTALL_RECEIPT_NAME));
    ASSERT_EQ(0, remove(receipt));
    take_away_the_download(&artifact);

    install_report second = install_run(&request);
    ASSERT_EQ(install_ok, second.status);
    EXPECT_TRUE(second.already_installed);

    fixture_teardown(&fixture);
}

/* A toolchain is found by its record, since the directory it lands in is named
   after what the compiler said it is rather than after what was published. */
static bool has_a_gcc(void) {
    const char *argv[] = {"gcc", "--version", NULL};
    const process_result result = process_try(argv, NULL);
    return result.completed && result.exit_code == 0;
}

MOLTEST(install_downloads_nothing_for_a_toolchain_already_installed) {
    if (!gzip_installs_work() || !has_a_gcc())
        SKIP("curl, tar with gzip and a gcc are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char bin[256];
    snprintf(bin, sizeof bin, "%s/chain-stage/bin", fixture.root);
    ASSERT_TRUE(fs_make_dirs(bin));
    char driver[512];
    snprintf(driver, sizeof driver, "%s/clang", bin);
    ASSERT_TRUE(moltest_fake_program(driver, "exec gcc\n", NULL, 0));

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/clang.tar.gz", fixture.root);
    snprintf(stage, sizeof stage, "%s/chain-stage", fixture.root);
    ASSERT_TRUE(pack(stage, archive));
    registry_artifact artifact;
    ASSERT_TRUE(describe(archive, "clang", "1.0.0", registry_kind_toolchain, &artifact));
    snprintf(artifact.format, sizeof artifact.format, "%s", REGISTRY_FORMAT_TAR_GZ);

    const install_request request = { .artifact = &artifact };
    install_report first = install_run(&request);
    ASSERT_EQ(install_ok, first.status);
    EXPECT_FALSE(first.already_installed);

    take_away_the_download(&artifact);

    install_report second = install_run(&request);
    ASSERT_EQ(install_ok, second.status);
    EXPECT_TRUE(second.already_installed);
    EXPECT_STREQ(first.directory, second.directory);

    fixture_teardown(&fixture);
}
