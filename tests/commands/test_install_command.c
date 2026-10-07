#include <moltest.h>

#include <pickup/commands/install_command.h>
#include <pickup/exit_code.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/sources/registry_source.h>

#include <stdio.h>
#include <stdlib.h>

/*
 * These cover only the paths install_command_run resolves before it ever
 * reaches the registry, since anything past that talks to the network and has
 * no place in a unit test.
 */

DESCRIBE(install_rejects_a_name_and_version_that_disagree) {
    /* "clang@22.1.0 --version 19" names the version twice; picking one over
       the other silently would install something other than what was asked
       for. */
    const install_command_request request = {
        .name = "clang@22.1.0",
        .version = "19",
    };
    EXPECT_EQ(exit_usage_error, install_command_run(&request));
}

DESCRIBE(install_rejects_a_name_and_version_even_when_they_agree) {
    /* Repeating the same version does not make the two spellings one
       request; it is still two answers to the same question. */
    const install_command_request request = {
        .name = "clang@22.1.0",
        .version = "22.1.0",
    };
    EXPECT_EQ(exit_usage_error, install_command_run(&request));
}

DESCRIBE(install_rejects_a_versioned_name_with_nothing_before_the_at) {
    /* "@22.1.0" splits into an empty name, which is not one the registry
       could ever publish. */
    const install_command_request request = {.name = "@22.1.0"};
    EXPECT_EQ(exit_usage_error, install_command_run(&request));
}

DESCRIBE(install_leaves_an_unversioned_name_alone) {
    /* No '@' at all is the common case, and must reach the usual name
       validation unmodified rather than being rejected for carrying a
       version it never named. */
    const install_command_request malformed = {.name = "../etc"};
    EXPECT_EQ(exit_usage_error, install_command_run(&malformed));
}

/* --- a cached catalogue that is behind the registry --- */

/*
 * A registry served from a directory over file://, with a cache that is fresh
 * and out of date at once: written seconds ago, before a release that the
 * registry already has. That is the hour after every publish, since the cache
 * is trusted for one, and "no clang-format matches version 2.0.0" for a version
 * that exists is the answer it used to give.
 */
typedef struct {
    char root[64];
    char previous_home[4096];
    char previous_url[4096];
    bool had_home;
    bool had_url;
} stale_fixture;

static void remember(const char *name, char *out, size_t size, bool *had) {
    const char *value = getenv(name);
    *had = value != NULL;
    if (value != NULL)
        snprintf(out, size, "%s", value);
}

static void restore(const char *name, const char *value, bool had) {
    if (had)
        (void)setenv(name, value, 1);
    else
        (void)unsetenv(name);
}

/* One release of clang-format per version in `versions`, for this machine. */
static bool write_releases(const char *path, const char *const *versions, size_t count) {
    char text[4096];
    size_t at = (size_t)snprintf(text, sizeof text,
                                 "{\"kind\":\"tool\",\"name\":\"clang-format\",\"releases\":[");
    for (size_t i = 0; i < count && at < sizeof text; i++) {
        at += (size_t)snprintf(
            text + at, sizeof text - at,
            "%s{\"kind\":\"tool\",\"name\":\"clang-format\",\"version\":\"%s\",\"targets\":[{"
            "\"kind\":\"tool\",\"name\":\"clang-format\",\"version\":\"%s\","
            "\"target\":\"%s\",\"format\":\"tar.gz\",\"checksum\":\"%064d\","
            "\"size_bytes\":1,\"yanked\":false,\"published_by\":null,"
            "\"metadata\":{\"tool\":{\"kind\":\"formatter\",\"binary\":\"bin/clang-format\"}},"
            "\"download_url\":\"file:///nowhere/cf.tar.gz\"}]}",
            i == 0 ? "" : ",", versions[i], versions[i], registry_host_target(), 0);
    }
    if (at + 3 >= sizeof text)
        return false;
    snprintf(text + at, sizeof text - at, "]}");
    return fs_write_file(path, text);
}

static bool stale_setup(stale_fixture *fixture, const char *cached_catalogue) {
    if (!moltest_temp_dir("pickup_stale", fixture->root, sizeof fixture->root))
        return false;
    remember(PICKUP_HOME_ENV, fixture->previous_home, sizeof fixture->previous_home,
             &fixture->had_home);
    remember(REGISTRY_URL_ENV, fixture->previous_url, sizeof fixture->previous_url,
             &fixture->had_url);

    char home[PICKUP_PATHS_MAX], url[PICKUP_PATHS_MAX];
    snprintf(home, sizeof home, "%s/home", fixture->root);
    snprintf(url, sizeof url, "file://%s/remote", fixture->root);
    if (setenv(PICKUP_HOME_ENV, home, 1) != 0 || setenv(REGISTRY_URL_ENV, url, 1) != 0)
        return false;

    /* What the cache was told before the release: 1.0.0 only. */
    char cache[PICKUP_PATHS_MAX], path[PICKUP_PATHS_MAX];
    if (!paths_cache(cache, sizeof cache) || !fs_make_dirs(cache))
        return false;
    snprintf(path, sizeof path, "%s/registry-tools.json", cache);
    if (!fs_write_file(path, cached_catalogue))
        return false;
    static const char *const before[] = {"1.0.0"};
    snprintf(path, sizeof path, "%s/registry-tools-clang-format.json", cache);
    if (!write_releases(path, before, 1))
        return false;

    /* What the registry has now: 1.0.0 and 2.0.0. */
    char remote[PICKUP_PATHS_MAX];
    snprintf(remote, sizeof remote, "%s/remote/v1/tools", fixture->root);
    if (!fs_make_dirs(remote))
        return false;
    static const char *const now[] = {"1.0.0", "2.0.0"};
    snprintf(path, sizeof path, "%s/clang-format", remote);
    return write_releases(path, now, 2);
}

static void stale_teardown(stale_fixture *fixture) {
    restore(PICKUP_HOME_ENV, fixture->previous_home, fixture->had_home);
    restore(REGISTRY_URL_ENV, fixture->previous_url, fixture->had_url);
    (void)fs_remove_tree(fixture->root);
}

static const char clang_format_catalogue[] =
    "{\"kind\":\"tool\",\"entries\":[{\"name\":\"clang-format\",\"latest_version\":\"1.0.0\","
    "\"versions\":1,\"targets\":[\"linux-x86_64\"],\"published_by\":null}]}";

/* A version the cache has not heard of is asked about once more, freshly,
   before it is called missing. */
DESCRIBE(install_asks_the_registry_again_before_saying_a_version_is_missing) {
    if (!http_available() || registry_host_target()[0] == '\0')
        SKIP("curl and a target the registry publishes for are needed");

    stale_fixture fixture;
    ASSERT_TRUE(stale_setup(&fixture, clang_format_catalogue));

    const install_command_request request = {.name = "clang-format@2.0.0", .dry_run = true};
    EXPECT_EQ(exit_ok, install_command_run(&request));

    stale_teardown(&fixture);
}

/* And a version that is in the cache costs no second request: the registry is
   only asked again when the cache could not answer. */
DESCRIBE(install_trusts_a_cache_that_has_the_answer) {
    if (!http_available() || registry_host_target()[0] == '\0')
        SKIP("curl and a target the registry publishes for are needed");

    stale_fixture fixture;
    ASSERT_TRUE(stale_setup(&fixture, clang_format_catalogue));
    /* Take the registry away: an answer can now only come from the cache. */
    char remote[PICKUP_PATHS_MAX];
    snprintf(remote, sizeof remote, "%s/remote", fixture.root);
    ASSERT_TRUE(fs_remove_tree(remote));

    const install_command_request request = {.name = "clang-format@1.0.0", .dry_run = true};
    EXPECT_EQ(exit_ok, install_command_run(&request));

    stale_teardown(&fixture);
}

/*
 * The same for a name: one the cached catalogues do not list is looked up
 * freshly before it is called unpublished.
 *
 * A directory served over file:// cannot hold a catalogue and the releases
 * beneath it at one path, so the fresh catalogue here is the toolchains one,
 * and describing the release afterwards fails. That is still a different
 * answer from "nothing is published as" — the exit code that says the name
 * does not exist — and that difference is what is checked.
 */
DESCRIBE(install_asks_the_registry_again_before_saying_a_name_is_unknown) {
    if (!http_available() || registry_host_target()[0] == '\0')
        SKIP("curl and a target the registry publishes for are needed");

    stale_fixture fixture;
    ASSERT_TRUE(stale_setup(&fixture, "{\"kind\":\"tool\",\"entries\":[]}"));

    /* Cached: no toolchain called newchain. The registry: one. */
    char cache[PICKUP_PATHS_MAX], path[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    snprintf(path, sizeof path, "%s/registry-toolchains.json", cache);
    ASSERT_TRUE(fs_write_file(path, "{\"kind\":\"toolchain\",\"entries\":[]}"));
    snprintf(path, sizeof path, "%s/remote/v1/toolchains", fixture.root);
    ASSERT_TRUE(fs_write_file(path,
                              "{\"kind\":\"toolchain\",\"entries\":[{\"name\":\"newchain\","
                              "\"latest_version\":\"1.0.0\",\"versions\":1,"
                              "\"targets\":[\"linux-x86_64\"],\"published_by\":null}]}"));

    const install_command_request request = {.name = "newchain", .dry_run = true};
    EXPECT_NE(exit_no_match, install_command_run(&request));

    stale_teardown(&fixture);
}
