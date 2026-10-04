#include <moltest.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>

#include "user_dirs_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every path Pickup writes to hangs off one root, so relocating that root is
   the only thing a test has to do to stay out of a real home directory. */
typedef struct {
    char previous[PICKUP_PATHS_MAX];
    bool had_previous;
} home_fixture;

static bool fixture_setup(home_fixture *fixture, const char *home) {
    const char *existing = getenv(PICKUP_HOME_ENV);
    fixture->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(fixture->previous, sizeof fixture->previous, "%s", existing);
    return setenv(PICKUP_HOME_ENV, home, 1) == 0;
}

static void fixture_teardown(home_fixture *fixture) {
    if (fixture->had_previous)
        (void)setenv(PICKUP_HOME_ENV, fixture->previous, 1);
    else
        (void)unsetenv(PICKUP_HOME_ENV);
}

MOLTEST(paths_put_everything_under_one_root) {
    home_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "/tmp/pickup-home-under-test"));

    char home[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_home(home, sizeof home));
    EXPECT_STREQ("/tmp/pickup-home-under-test", home);

    char cache[PICKUP_PATHS_MAX];
    char downloads[PICKUP_PATHS_MAX];
    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));

    EXPECT_STREQ("/tmp/pickup-home-under-test/cache", cache);
    EXPECT_STREQ("/tmp/pickup-home-under-test/downloads", downloads);
    EXPECT_STREQ("/tmp/pickup-home-under-test/toolchains", toolchains);

    fixture_teardown(&fixture);
}

MOLTEST(paths_keep_the_disposable_apart_from_the_installed) {
    home_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "/tmp/pickup-home-under-test"));

    char cache[PICKUP_PATHS_MAX];
    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));

    /* Deleting the cache must never be able to take an installed toolchain
       with it, so neither may sit inside the other. */
    EXPECT_TRUE(strstr(toolchains, cache) == NULL);
    EXPECT_TRUE(strstr(cache, toolchains) == NULL);

    fixture_teardown(&fixture);
}

MOLTEST(paths_name_a_toolchain_by_what_it_is) {
    home_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "/tmp/pickup-home-under-test"));

    char directory[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchain_dir("clang", "22.1.8", "x86_64-unknown-linux-gnu",
                                    directory, sizeof directory));

    /* Vendor, version and target together, so two versions, or one version
       built for two targets, never land on the same directory. */
    EXPECT_STREQ("/tmp/pickup-home-under-test/toolchains/"
                 "clang-22.1.8-x86_64-unknown-linux-gnu", directory);

    char other[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchain_dir("clang", "22.1.7", "x86_64-unknown-linux-gnu",
                                    other, sizeof other));
    EXPECT_TRUE(strcmp(directory, other) != 0);

    fixture_teardown(&fixture);
}

MOLTEST(paths_find_the_toolchain_a_binary_belongs_to) {
    home_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "/tmp/pickup-home-under-test"));

    /* What `uninstall` removes is the whole toolchain, not the directory the
       binary happens to sit in. */
    char owner[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_owning_toolchain(
        "/tmp/pickup-home-under-test/toolchains/clang-22.1.8-x86_64-unknown-linux-gnu/bin/clang",
        owner, sizeof owner));
    EXPECT_STREQ("/tmp/pickup-home-under-test/toolchains/"
                 "clang-22.1.8-x86_64-unknown-linux-gnu", owner);

    fixture_teardown(&fixture);
}

MOLTEST(paths_claim_nothing_outside_the_toolchains_directory) {
    home_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture, "/tmp/pickup-home-under-test"));

    char owner[PICKUP_PATHS_MAX];

    /* A compiler the package manager owns. Answering with anything here would
       be answering with a path to delete. */
    EXPECT_FALSE(paths_owning_toolchain("/usr/bin/gcc-12", owner, sizeof owner));

    /* A prefix match alone would claim this one, which is a different
       directory that merely starts with the same characters. */
    EXPECT_FALSE(paths_owning_toolchain(
        "/tmp/pickup-home-under-test/toolchains-old/clang-1/bin/clang",
        owner, sizeof owner));

    /* The toolchains directory itself belongs to no single toolchain. */
    EXPECT_FALSE(paths_owning_toolchain("/tmp/pickup-home-under-test/toolchains/",
                                        owner, sizeof owner));

    fixture_teardown(&fixture);
}

MOLTEST(paths_fall_back_to_the_users_home) {
    home_fixture fixture;
    /* Unset rather than pointed somewhere: this is the default users get. */
    ASSERT_TRUE(fixture_setup(&fixture, "/tmp/ignored"));
    (void)unsetenv(PICKUP_HOME_ENV);

    char home[PICKUP_PATHS_MAX];
    const char *user_home = getenv("HOME");
    if (user_home == NULL)
        SKIP("HOME is not set");

    ASSERT_TRUE(paths_home(home, sizeof home));
    char expected[PICKUP_PATHS_MAX];
    snprintf(expected, sizeof expected, "%s/%s", user_home, PICKUP_HOME_DIRNAME);
    EXPECT_STREQ(expected, home);

    fixture_teardown(&fixture);
}

/* --- the platform directories, when nothing is relocated --- */

/* `path` equals the fixture root followed by `suffix`. */
static bool is_under(const user_dirs_fixture *fixture, const char *path, const char *suffix) {
    char expected[PICKUP_PATHS_MAX];
    snprintf(expected, sizeof expected, "%s%s", fixture->root, suffix);
    if (strcmp(expected, path) == 0)
        return true;
    fprintf(stderr, "    expected %s\n         got %s\n", expected, path);
    return false;
}

/* Every role asked at once, so each test states the whole layout. */
typedef struct {
    char config[PICKUP_PATHS_MAX];
    char data[PICKUP_PATHS_MAX];
    char toolchains[PICKUP_PATHS_MAX];
    char tools[PICKUP_PATHS_MAX];
    char cache[PICKUP_PATHS_MAX];
    char downloads[PICKUP_PATHS_MAX];
} layout;

static bool layout_of(layout *out) {
    return paths_config(out->config, sizeof out->config) &&
           paths_data(out->data, sizeof out->data) &&
           paths_toolchains(out->toolchains, sizeof out->toolchains) &&
           paths_tools(out->tools, sizeof out->tools) &&
           paths_cache(out->cache, sizeof out->cache) &&
           paths_downloads(out->downloads, sizeof out->downloads);
}

#ifndef _WIN32
MOLTEST(paths_follow_the_xdg_variables_when_they_are_set) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_xdg"));

    layout found;
    ASSERT_TRUE(layout_of(&found));
    EXPECT_TRUE(is_under(&fixture, found.config, "/xdg/config/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.data, "/xdg/data/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.toolchains, "/xdg/data/pickup/toolchains"));
    EXPECT_TRUE(is_under(&fixture, found.tools, "/xdg/data/pickup/tools"));
    EXPECT_TRUE(is_under(&fixture, found.cache, "/xdg/cache/pickup"));
    /* Archives in flight are the most disposable thing there is. */
    EXPECT_TRUE(is_under(&fixture, found.downloads, "/xdg/cache/pickup/downloads"));

    user_dirs_teardown(&fixture);
}

MOLTEST(paths_fall_back_to_the_xdg_defaults_when_the_variables_are_unset) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_xdg"));
    ASSERT_TRUE(user_dirs_point(&fixture, "XDG_CONFIG_HOME", NULL));
    ASSERT_TRUE(user_dirs_point(&fixture, "XDG_DATA_HOME", NULL));
    ASSERT_TRUE(user_dirs_point(&fixture, "XDG_CACHE_HOME", NULL));

    /* macOS included: the user asked for the same layout there, not
       ~/Library, so this test runs on both. */
    layout found;
    ASSERT_TRUE(layout_of(&found));
    EXPECT_TRUE(is_under(&fixture, found.config, "/home/.config/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.toolchains, "/home/.local/share/pickup/toolchains"));
    EXPECT_TRUE(is_under(&fixture, found.tools, "/home/.local/share/pickup/tools"));
    EXPECT_TRUE(is_under(&fixture, found.cache, "/home/.cache/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.downloads, "/home/.cache/pickup/downloads"));

    user_dirs_teardown(&fixture);
}

MOLTEST(paths_ignore_an_empty_xdg_variable) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_xdg"));
    ASSERT_TRUE(setenv("XDG_CONFIG_HOME", "", 1) == 0);
    ASSERT_TRUE(setenv("XDG_DATA_HOME", "", 1) == 0);
    ASSERT_TRUE(setenv("XDG_CACHE_HOME", "", 1) == 0);

    /* The spec says an empty value is the same as an unset one. */
    layout found;
    ASSERT_TRUE(layout_of(&found));
    EXPECT_TRUE(is_under(&fixture, found.config, "/home/.config/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.data, "/home/.local/share/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.cache, "/home/.cache/pickup"));

    user_dirs_teardown(&fixture);
}

MOLTEST(paths_ignore_a_relative_xdg_variable) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_xdg"));
    ASSERT_TRUE(setenv("XDG_CONFIG_HOME", "relative/config", 1) == 0);
    ASSERT_TRUE(setenv("XDG_DATA_HOME", "./data", 1) == 0);
    ASSERT_TRUE(setenv("XDG_CACHE_HOME", "cache", 1) == 0);

    /* And a relative one is invalid and ignored: honouring it would put the
       toolchains wherever the command happened to be run from. */
    layout found;
    ASSERT_TRUE(layout_of(&found));
    EXPECT_TRUE(is_under(&fixture, found.config, "/home/.config/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.data, "/home/.local/share/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.cache, "/home/.cache/pickup"));

    user_dirs_teardown(&fixture);
}

MOLTEST(paths_do_not_double_a_trailing_separator) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_xdg"));
    ASSERT_TRUE(user_dirs_point(&fixture, "XDG_DATA_HOME", "/xdg/data/"));

    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));
    /* A prefix comparison against a doubled slash would disown every
       toolchain installed under it. */
    EXPECT_TRUE(is_under(&fixture, toolchains, "/xdg/data/pickup/toolchains"));

    user_dirs_teardown(&fixture);
}

MOLTEST(paths_know_nothing_without_a_home_or_xdg_variables) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_xdg"));
    ASSERT_TRUE(user_dirs_point(&fixture, "HOME", NULL));
    ASSERT_TRUE(user_dirs_point(&fixture, "XDG_DATA_HOME", NULL));

    char toolchains[PICKUP_PATHS_MAX];
    EXPECT_FALSE(paths_toolchains(toolchains, sizeof toolchains));

    /* A role whose variable is set still answers: each stands on its own. */
    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_config(config, sizeof config));
    EXPECT_TRUE(is_under(&fixture, config, "/xdg/config/pickup"));

    user_dirs_teardown(&fixture);
}

MOLTEST(paths_find_the_toolchain_a_binary_belongs_to_under_xdg) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_xdg"));

    char driver[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(driver, sizeof driver, "%s/xdg/data/pickup/toolchains/clang-1-x/bin/clang",
             fixture.root));
    char owner[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_owning_toolchain(driver, owner, sizeof owner));
    EXPECT_TRUE(is_under(&fixture, owner, "/xdg/data/pickup/toolchains/clang-1-x"));

    /* What is still in the legacy directory was not installed into the
       current one, and `uninstall` does not reach into it. */
    ASSERT_TRUE(fs_format_path(driver, sizeof driver, "%s/home/.pickup/toolchains/clang-1-x/bin/clang",
             fixture.root));
    EXPECT_FALSE(paths_owning_toolchain(driver, owner, sizeof owner));

    user_dirs_teardown(&fixture);
}
#endif

#ifdef _WIN32
MOLTEST(paths_keep_everything_under_appdata_on_windows) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_appdata"));

    /* The XDG variables mean nothing to Windows; a shell that brings its own
       environment may still set them, and they are not obeyed. */
    layout found;
    ASSERT_TRUE(layout_of(&found));
    EXPECT_TRUE(is_under(&fixture, found.config, "/appdata/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.data, "/appdata/pickup"));
    EXPECT_TRUE(is_under(&fixture, found.toolchains, "/appdata/pickup/toolchains"));
    EXPECT_TRUE(is_under(&fixture, found.tools, "/appdata/pickup/tools"));
    EXPECT_TRUE(is_under(&fixture, found.cache, "/appdata/pickup/cache"));
    EXPECT_TRUE(is_under(&fixture, found.downloads, "/appdata/pickup/cache/downloads"));

    user_dirs_teardown(&fixture);
}

MOLTEST(paths_know_nothing_without_appdata_on_windows) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_appdata"));
    ASSERT_TRUE(user_dirs_point(&fixture, "APPDATA", NULL));

    char toolchains[PICKUP_PATHS_MAX];
    EXPECT_FALSE(paths_toolchains(toolchains, sizeof toolchains));

    user_dirs_teardown(&fixture);
}
#endif

MOLTEST(paths_home_overrides_every_platform_directory) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_relocated"));
    ASSERT_TRUE(user_dirs_point(&fixture, PICKUP_HOME_ENV, "/relocated"));
    EXPECT_TRUE(paths_relocated());

    /* The single-directory layout, exactly as before the split: the whole
       suite stands on it. */
    layout found;
    ASSERT_TRUE(layout_of(&found));
    EXPECT_TRUE(is_under(&fixture, found.config, "/relocated"));
    EXPECT_TRUE(is_under(&fixture, found.data, "/relocated"));
    EXPECT_TRUE(is_under(&fixture, found.toolchains, "/relocated/toolchains"));
    EXPECT_TRUE(is_under(&fixture, found.tools, "/relocated/tools"));
    EXPECT_TRUE(is_under(&fixture, found.cache, "/relocated/cache"));
    EXPECT_TRUE(is_under(&fixture, found.downloads, "/relocated/downloads"));

    /* And the legacy directory is still the user's, whatever is relocated. */
    char legacy[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_legacy_home(legacy, sizeof legacy));
    EXPECT_TRUE(is_under(&fixture, legacy, "/home/.pickup"));

    user_dirs_teardown(&fixture);
}

MOLTEST(paths_are_not_relocated_by_an_empty_pickup_home) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_relocated"));
    ASSERT_TRUE(setenv(PICKUP_HOME_ENV, "", 1) == 0);
    EXPECT_FALSE(paths_relocated());

    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));
#ifdef _WIN32
    EXPECT_TRUE(is_under(&fixture, toolchains, "/appdata/pickup/toolchains"));
#else
    EXPECT_TRUE(is_under(&fixture, toolchains, "/xdg/data/pickup/toolchains"));
#endif

    user_dirs_teardown(&fixture);
}
