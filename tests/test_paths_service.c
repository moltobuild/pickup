#include <moltest.h>

#include <pickup/services/paths_service.h>

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
