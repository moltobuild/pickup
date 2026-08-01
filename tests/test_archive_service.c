#include <moltest.h>

#include <pickup/services/archive_service.h>
#include <pickup/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char root[64];
} archive_fixture;

static bool fixture_setup(archive_fixture *fixture) {
    snprintf(fixture->root, sizeof fixture->root, "%s", "/tmp/pickup_tar_XXXXXX");
    return mkdtemp(fixture->root) != NULL;
}

static void fixture_teardown(archive_fixture *fixture) {
    (void)fs_remove_tree(fixture->root);
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

    char command[1024];
    snprintf(command, sizeof command, "tar -czf %s -C %s LLVM-1.0.0-Linux-X64", archive, root);
    return system(command) == 0;
}

MOLTEST(archive_reports_whether_it_can_extract) {
    bool first = archive_available();
    EXPECT_TRUE(archive_available() == first);
    EXPECT_STREQ("tar", archive_requirement());
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
