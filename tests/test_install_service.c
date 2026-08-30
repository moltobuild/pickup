#include <moltest.h>

#include <pickup/services/archive_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/install_service.h>
#include <pickup/util/sha256.h>

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
static bool pack(const char *stage, const char *archive) {
    char command[1024];
    snprintf(command, sizeof command, "tar -C %s -caf %s .", stage, archive);
    return system(command) == 0;
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
    if (!fs_write_file(driver, "#!/bin/sh\nexec /usr/bin/gcc-12 \"$@\"\n"))
        return false;
    if (chmod(driver, 0755) != 0)
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
    snprintf(artifact.format, sizeof artifact.format, "%s", "tar.gz");
    snprintf(artifact.download_url, sizeof artifact.download_url, "%s",
             "file:///nowhere");

    /* How a blob is packed is something the registry states, and this build
       opens one packing. Refused on the statement, before anything is fetched
       and without inferring anything from the URL. */
    const install_request request = { .artifact = &artifact };
    install_report report = install_run(&request);
    EXPECT_EQ(install_unsupported_format, report.status);

    char downloads[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    EXPECT_FALSE(fs_path_exists(downloads));

    fixture_teardown(&fixture);
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

    char binary[512];
    snprintf(binary, sizeof binary, "%s/clang-format", bin);
    ASSERT_TRUE(fs_write_file(binary, "#!/bin/sh\necho 'clang-format version 1.0.0'\n"));
    ASSERT_EQ(0, chmod(binary, 0755));

    char archive[256], stage[256];
    snprintf(archive, sizeof archive, "%s/cf.tar.zst", fixture.root);
    snprintf(stage, sizeof stage, "%s/stage", fixture.root);
    ASSERT_TRUE(pack(stage, archive));

    registry_artifact artifact;
    ASSERT_TRUE(describe(archive, "clang-format", "1.0.0", registry_kind_tool, &artifact));
    snprintf(artifact.binary, sizeof artifact.binary, "%s", "bin/clang-format");

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
    ASSERT_TRUE(fs_write_file(other, "#!/bin/sh\nexit 0\n"));
    ASSERT_EQ(0, chmod(other, 0755));

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
        install_ok, install_no_downloader, install_no_extractor, install_no_zstd,
        install_unsupported_format, install_yanked, install_download_failed,
        install_hash_mismatch, install_extract_failed, install_not_a_toolchain,
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
