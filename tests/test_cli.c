#include <moltest.h>

#include <pickup/cli.h>
#include <pickup/services/fs_service.h>

#include <stdlib.h>
#include <string.h>

/*
 * What `-V` reports has to be what the manifest declares.
 *
 * The number lives in `Project.toml` alone now: molto passes it to every
 * translation unit it compiles, and the bootstrap Makefile reads the same line
 * out of the same file. So this no longer catches two files drifting apart --
 * it catches the mechanism failing, and it catches a stale binary, which looks
 * exactly the same from outside and is the one question `-V` exists to settle.
 */

/* The manifest sits at the root, which is where `make test` runs from. */
#define MANIFEST_PATH "Project.toml"
#define PACKAGE_SECTION "[package]"
#define VERSION_KEY "version = \""

/* Read the version `[package]` declares. False when the file says nothing about
   it, which is a different answer from disagreeing. */
static bool manifest_version(const char *text, char *out, size_t out_size) {
    const char *section = strstr(text, PACKAGE_SECTION);
    if (section == NULL)
        return false;

    const char *key = strstr(section, VERSION_KEY);
    if (key == NULL)
        return false;
    key += sizeof VERSION_KEY - 1;

    const char *end = strchr(key, '"');
    if (end == NULL)
        return false;

    size_t length = (size_t)(end - key);
    if (length == 0 || length >= out_size)
        return false;
    memcpy(out, key, length);
    out[length] = '\0';
    return true;
}

MOLTEST(cli_reports_the_version_the_manifest_declares) {
    char *text = fs_read_file(MANIFEST_PATH);
    if (text == NULL)
        SKIP("the manifest is only there when the suite runs from the repository root");

    char declared[64];
    bool read = manifest_version(text, declared, sizeof declared);
    free(text);
    ASSERT_TRUE(read);

    /* What `pickup -V` prints, and what a person compares against the release
       they think they installed. */
    EXPECT_STREQ(declared, cli_version());
}

MOLTEST(cli_version_is_not_empty) {
    const char *version = cli_version();
    ASSERT_NOT_NULL(version);
    EXPECT_TRUE(version[0] != '\0');
}
