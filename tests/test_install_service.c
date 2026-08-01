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
   download, digest, unpack, identify, rename — without a network or a
   gigabyte. The compiler inside the archive is a wrapper around the real one,
   because the last step asks it to identify itself and nothing else would. */
typedef struct {
    char root[64];
    char previous_home[4096];
    bool had_home;
} install_fixture;

static bool fixture_setup(install_fixture *fixture) {
    snprintf(fixture->root, sizeof fixture->root, "%s", "/tmp/pickup_install_XXXXXX");
    if (mkdtemp(fixture->root) == NULL)
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

/* Build an archive holding a working compiler at bin/clang, and describe it
   the way the source would. */
static bool make_release(install_fixture *fixture, release_asset *asset, bool with_digest) {
    char inner[256];
    snprintf(inner, sizeof inner, "%s/stage/LLVM-1.0.0-Linux-X64/bin", fixture->root);
    if (!fs_make_dirs(inner))
        return false;

    char driver[512];
    snprintf(driver, sizeof driver, "%s/clang", inner);
    if (!fs_write_file(driver, "#!/bin/sh\nexec /usr/bin/gcc-12 \"$@\"\n"))
        return false;
    if (chmod(driver, 0755) != 0)
        return false;

    char archive[256], stage[256], command[1024];
    snprintf(archive, sizeof archive, "%s/toolchain.tar.gz", fixture->root);
    snprintf(stage, sizeof stage, "%s/stage", fixture->root);
    snprintf(command, sizeof command, "tar -czf %s -C %s LLVM-1.0.0-Linux-X64",
             archive, stage);
    if (system(command) != 0)
        return false;

    *asset = (release_asset){ 0 };
    snprintf(asset->version, sizeof asset->version, "%s", "1.0.0");
    snprintf(asset->tag, sizeof asset->tag, "%s", "llvmorg-1.0.0");
    snprintf(asset->asset, sizeof asset->asset, "%s", "toolchain.tar.gz");
    snprintf(asset->url, sizeof asset->url, "file://%s", archive);

    long long size = 0;
    if (!fs_file_size(archive, &size))
        return false;
    asset->size = size;

    if (with_digest && !sha256_file(archive, asset->sha256))
        return false;
    return true;
}

static bool tools_present(void) {
    return http_available() && archive_available() && access("/usr/bin/gcc-12", X_OK) == 0;
}

MOLTEST(install_places_a_verified_toolchain_under_the_pickup_home) {
    if (!tools_present())
        SKIP("curl, tar and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    release_asset asset;
    ASSERT_TRUE(make_release(&fixture, &asset, true));

    install_request request = { .asset = &asset, .allow_unverified = false };
    install_report report = install_run(&request);

    ASSERT_EQ(install_ok, report.status);
    EXPECT_TRUE(install_succeeded(report.status));

    /* Installed under the pickup home, never anywhere needing privileges. */
    char home[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_home(home, sizeof home));
    EXPECT_TRUE(strncmp(report.directory, home, strlen(home)) == 0);

    /* The compiler is where the scanner will look for it. */
    char driver[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(driver, sizeof driver, "%s/bin/clang", report.directory));
    EXPECT_TRUE(fs_path_exists(driver));

    /* The directory is named from what the compiler said it is, not from what
       the archive was called. */
    EXPECT_TRUE(strstr(report.directory, "LLVM-1.0.0") == NULL);
    EXPECT_TRUE(report.installed.version.major > 0);
    EXPECT_TRUE(strlen(report.installed.target) > 0);

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
        SKIP("curl, tar and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    release_asset asset;
    ASSERT_TRUE(make_release(&fixture, &asset, true));
    /* One byte of the expected digest changed: a tampered or truncated
       download must never be unpacked. */
    asset.sha256[0] = asset.sha256[0] == 'a' ? 'b' : 'a';

    install_request request = { .asset = &asset, .allow_unverified = false };
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

MOLTEST(install_refuses_what_it_cannot_verify) {
    if (!tools_present())
        SKIP("curl, tar and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    release_asset asset;
    ASSERT_TRUE(make_release(&fixture, &asset, false)); /* no digest published */

    install_request request = { .asset = &asset, .allow_unverified = false };
    install_report report = install_run(&request);

    EXPECT_EQ(install_unverifiable, report.status);

    /* Refused before spending the download, not after. */
    char downloads[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    char archive[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(archive, sizeof archive, "%s/%s", downloads, asset.asset));
    EXPECT_FALSE(fs_path_exists(archive));

    fixture_teardown(&fixture);
}

MOLTEST(install_proceeds_unverified_when_explicitly_allowed) {
    if (!tools_present())
        SKIP("curl, tar and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    release_asset asset;
    ASSERT_TRUE(make_release(&fixture, &asset, false));

    install_request request = { .asset = &asset, .allow_unverified = true };
    install_report report = install_run(&request);

    /* It installs, and says plainly that it could not be checked. */
    EXPECT_EQ(install_ok_unverified, report.status);
    EXPECT_TRUE(install_succeeded(report.status));
    EXPECT_TRUE(fs_path_exists(report.directory));

    fixture_teardown(&fixture);
}

MOLTEST(install_rejects_an_archive_without_a_compiler_in_it) {
    if (!tools_present())
        SKIP("curl, tar and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    /* An archive that unpacks fine but holds nothing that identifies itself. */
    char inner[256];
    snprintf(inner, sizeof inner, "%s/stage/LLVM-1.0.0-Linux-X64/share", fixture.root);
    ASSERT_TRUE(fs_make_dirs(inner));
    char readme[512];
    snprintf(readme, sizeof readme, "%s/README", inner);
    ASSERT_TRUE(fs_write_file(readme, "not a compiler\n"));

    char archive[256], stage[256], command[1024];
    snprintf(archive, sizeof archive, "%s/empty.tar.gz", fixture.root);
    snprintf(stage, sizeof stage, "%s/stage", fixture.root);
    snprintf(command, sizeof command, "tar -czf %s -C %s LLVM-1.0.0-Linux-X64",
             archive, stage);
    ASSERT_EQ(0, system(command));

    release_asset asset = { 0 };
    snprintf(asset.version, sizeof asset.version, "%s", "1.0.0");
    snprintf(asset.asset, sizeof asset.asset, "%s", "empty.tar.gz");
    snprintf(asset.url, sizeof asset.url, "file://%s", archive);
    ASSERT_TRUE(sha256_file(archive, asset.sha256));

    install_request request = { .asset = &asset, .allow_unverified = false };
    install_report report = install_run(&request);

    EXPECT_EQ(install_not_a_toolchain, report.status);

    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));
    char partial[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(partial, sizeof partial, "%s/.partial", toolchains));
    EXPECT_FALSE(fs_path_exists(partial));

    fixture_teardown(&fixture);
}

/* Build an archive with a working compiler plus the kind of bulk a real
   release carries and Molto never uses. */
static bool make_bulky_release(install_fixture *fixture, release_asset *asset) {
    const char *bulk[] = {
        "bin/mlir-opt", "bin/flang-1", "lib/libLLVMCore.a", "share/doc/readme",
        "lib/clang/1/lib/libflang_rt.runtime.a",
    };
    char stage[256];
    snprintf(stage, sizeof stage, "%s/bulky/LLVM-1.0.0-Linux-X64", fixture->root);

    char bin[512];
    snprintf(bin, sizeof bin, "%s/bin", stage);
    if (!fs_make_dirs(bin))
        return false;
    char driver[640];
    snprintf(driver, sizeof driver, "%s/clang", bin);
    if (!fs_write_file(driver, "#!/bin/sh\nexec /usr/bin/gcc-12 \"$@\"\n")
        || chmod(driver, 0755) != 0)
        return false;

    for (size_t i = 0; i < sizeof bulk / sizeof bulk[0]; i++) {
        char path[640];
        snprintf(path, sizeof path, "%s/%s", stage, bulk[i]);
        char *slash = strrchr(path, '/');
        *slash = '\0';
        if (!fs_make_dirs(path))
            return false;
        *slash = '/';
        if (!fs_write_file(path, "bulk"))
            return false;
    }

    char archive[256], root[256], command[1024];
    snprintf(archive, sizeof archive, "%s/bulky.tar.gz", fixture->root);
    snprintf(root, sizeof root, "%s/bulky", fixture->root);
    snprintf(command, sizeof command, "tar -czf %s -C %s LLVM-1.0.0-Linux-X64",
             archive, root);
    if (system(command) != 0)
        return false;

    *asset = (release_asset){ 0 };
    snprintf(asset->version, sizeof asset->version, "%s", "1.0.0");
    snprintf(asset->asset, sizeof asset->asset, "%s", "bulky.tar.gz");
    snprintf(asset->url, sizeof asset->url, "file://%s", archive);
    long long size = 0;
    if (!fs_file_size(archive, &size))
        return false;
    asset->size = size;
    return sha256_file(archive, asset->sha256);
}

MOLTEST(install_keeps_only_what_the_ecosystem_uses) {
    if (!tools_present())
        SKIP("curl, tar and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    release_asset asset;
    ASSERT_TRUE(make_bulky_release(&fixture, &asset));

    install_request request = { .asset = &asset, .allow_unverified = false };
    install_report report = install_run(&request);
    ASSERT_EQ(install_ok, report.status);

    /* The compiler is there and it compiles: the count is what says the
       pruning did not quietly break it. */
    EXPECT_TRUE(report.features_proven > 0);
    char path[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/bin/clang", report.directory));
    EXPECT_TRUE(fs_path_exists(path));

    /* And the rest of the release never reached the disk. */
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/bin/mlir-opt", report.directory));
    EXPECT_FALSE(fs_path_exists(path));
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/lib/libLLVMCore.a", report.directory));
    EXPECT_FALSE(fs_path_exists(path));
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/share/doc/readme", report.directory));
    EXPECT_FALSE(fs_path_exists(path));
    ASSERT_TRUE(fs_format_path(path, sizeof path,
                               "%s/lib/clang/1/lib/libflang_rt.runtime.a", report.directory));
    EXPECT_FALSE(fs_path_exists(path));

    EXPECT_TRUE(report.installed_size > 0);
    fixture_teardown(&fixture);
}

MOLTEST(install_falls_back_to_the_whole_release_when_pruning_breaks_it) {
    if (!tools_present())
        SKIP("curl, tar and gcc-12 are needed for an end to end install");

    install_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    /* A compiler that needs something the profile does not collect, which is
       exactly how a real profile fails: bin/clang is kept, but what it depends
       on lives somewhere the patterns do not reach. */
    char stage[256], bin[512], support[512];
    snprintf(stage, sizeof stage, "%s/split/LLVM-1.0.0-Linux-X64", fixture.root);
    snprintf(bin, sizeof bin, "%s/bin", stage);
    snprintf(support, sizeof support, "%s/share/support", stage);
    ASSERT_TRUE(fs_make_dirs(bin));
    ASSERT_TRUE(fs_make_dirs(support));

    char real[640], driver[640];
    snprintf(real, sizeof real, "%s/real-compiler", support);
    snprintf(driver, sizeof driver, "%s/clang", bin);
    ASSERT_TRUE(fs_write_file(real, "#!/bin/sh\nexec /usr/bin/gcc-12 \"$@\"\n"));
    ASSERT_EQ(0, chmod(real, 0755));
    ASSERT_TRUE(fs_write_file(driver,
        "#!/bin/sh\nexec \"$(dirname \"$0\")/../share/support/real-compiler\" \"$@\"\n"));
    ASSERT_EQ(0, chmod(driver, 0755));

    char archive[256], root[256], command[1024];
    snprintf(archive, sizeof archive, "%s/split.tar.gz", fixture.root);
    snprintf(root, sizeof root, "%s/split", fixture.root);
    snprintf(command, sizeof command, "tar -czf %s -C %s LLVM-1.0.0-Linux-X64",
             archive, root);
    ASSERT_EQ(0, system(command));

    release_asset asset = { 0 };
    snprintf(asset.version, sizeof asset.version, "%s", "1.0.0");
    snprintf(asset.asset, sizeof asset.asset, "%s", "split.tar.gz");
    snprintf(asset.url, sizeof asset.url, "file://%s", archive);
    ASSERT_TRUE(sha256_file(archive, asset.sha256));

    install_request request = { .asset = &asset, .allow_unverified = false };
    install_report report = install_run(&request);

    /* Pruned, found not to compile, and unpacked whole instead of being left
       installed and broken. */
    EXPECT_EQ(install_ok_unpruned, report.status);
    EXPECT_TRUE(install_succeeded(report.status));
    EXPECT_TRUE(report.features_proven > 0);

    char path[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/share/support/real-compiler",
                               report.directory));
    EXPECT_TRUE(fs_path_exists(path));

    fixture_teardown(&fixture);
}

MOLTEST(install_status_messages_cover_every_outcome) {
    /* A caller prints these; none may come out blank. */
    const install_status all[] = {
        install_ok, install_ok_unverified, install_ok_unpruned, install_no_downloader,
        install_no_extractor, install_unverifiable, install_download_failed,
        install_hash_mismatch, install_extract_failed, install_not_a_toolchain,
        install_path_error,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *message = install_status_message(all[i]);
        EXPECT_TRUE(message != NULL && message[0] != '\0');
    }
    EXPECT_TRUE(install_succeeded(install_ok));
    EXPECT_TRUE(install_succeeded(install_ok_unverified));
    EXPECT_FALSE(install_succeeded(install_hash_mismatch));
}
