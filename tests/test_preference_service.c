#include <moltest.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/preference_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A throwaway pickup home, so a test never reads or writes the real one.
   Relocating the root is the whole of the isolation, as everywhere else. */
typedef struct {
    char root[64];
    char previous[PICKUP_PATHS_MAX];
    bool had_previous;
} preference_fixture;

static bool fixture_setup(preference_fixture *fixture) {
    if (!moltest_temp_dir("pickup_pref", fixture->root, sizeof fixture->root))
        return false;

    const char *existing = getenv(PICKUP_HOME_ENV);
    fixture->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(fixture->previous, sizeof fixture->previous, "%s", existing);
    return setenv(PICKUP_HOME_ENV, fixture->root, 1) == 0;
}

static void fixture_teardown(preference_fixture *fixture) {
    if (fixture->had_previous)
        (void)setenv(PICKUP_HOME_ENV, fixture->previous, 1);
    else
        (void)unsetenv(PICKUP_HOME_ENV);
    (void)fs_remove_tree(fixture->root);
}

/* The configuration file itself, for the tests that care what is in it. */
static bool config_path(const preference_fixture *fixture, char *out, size_t out_size) {
    return fs_format_path(out, out_size, "%s/config.toml", fixture->root);
}

MOLTEST(preference_reports_no_default_on_a_fresh_home) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    /* Not an error: it is the state every machine starts in. */
    char id[PREFERENCE_VALUE_MAX];
    EXPECT_FALSE(preference_default_get(id, sizeof id));

    fixture_teardown(&fixture);
}

MOLTEST(preference_round_trips_a_default) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    ASSERT_TRUE(preference_default_set("clang@22.1.9"));

    char id[PREFERENCE_VALUE_MAX];
    ASSERT_TRUE(preference_default_get(id, sizeof id));
    EXPECT_STREQ("clang@22.1.9", id);

    fixture_teardown(&fixture);
}

MOLTEST(preference_keeps_the_home_out_of_the_cache) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    ASSERT_TRUE(preference_default_set("gcc@12.3.0"));

    /* A preference is a decision, not something Pickup can rebuild. Storing it
       under the cache would have `pickup scan`, or anyone clearing disk space,
       silently discard it. */
    char cache[PICKUP_PATHS_MAX];
    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    ASSERT_TRUE(config_path(&fixture, config, sizeof config));
    EXPECT_TRUE(strstr(config, cache) == NULL);

    fixture_teardown(&fixture);
}

MOLTEST(preference_replaces_rather_than_appends) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    ASSERT_TRUE(preference_default_set("gcc@12.3.0"));
    ASSERT_TRUE(preference_default_set("clang@22.1.9"));

    char id[PREFERENCE_VALUE_MAX];
    ASSERT_TRUE(preference_default_get(id, sizeof id));
    EXPECT_STREQ("clang@22.1.9", id);

    /* Two assignments of one key is a file whose meaning depends on which one
       a reader stops at, so the old line has to be gone and not merely
       shadowed. */
    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(config_path(&fixture, config, sizeof config));
    char *text = fs_read_file(config);
    ASSERT_TRUE(text != NULL);
    EXPECT_TRUE(strstr(text, "gcc@12.3.0") == NULL);
    free(text);

    fixture_teardown(&fixture);
}

MOLTEST(preference_clears_and_stays_cleared) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    ASSERT_TRUE(preference_default_set("clang@22.1.9"));
    ASSERT_TRUE(preference_default_clear());

    char id[PREFERENCE_VALUE_MAX];
    EXPECT_FALSE(preference_default_get(id, sizeof id));

    /* Clearing twice is not an error: there is nothing left to fail at. */
    EXPECT_TRUE(preference_default_clear());

    fixture_teardown(&fixture);
}

MOLTEST(preference_preserves_what_it_did_not_write) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(config_path(&fixture, config, sizeof config));
    ASSERT_TRUE(fs_write_file(config,
                              "# written by hand\n"
                              "something_else = \"kept\"\n"));

    ASSERT_TRUE(preference_default_set("gcc@12.3.0"));

    /* A configuration file is something a person may have edited. Rewriting
       only the keys this version understands would delete the rest. */
    char *text = fs_read_file(config);
    ASSERT_TRUE(text != NULL);
    EXPECT_TRUE(strstr(text, "# written by hand") != NULL);
    EXPECT_TRUE(strstr(text, "something_else = \"kept\"") != NULL);
    EXPECT_TRUE(strstr(text, "gcc@12.3.0") != NULL);
    free(text);

    fixture_teardown(&fixture);
}

MOLTEST(preference_ignores_a_value_it_did_not_write) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(config_path(&fixture, config, sizeof config));
    /* Unquoted, which is not the shape Pickup writes. Guessing at it would
       mean acting on a name nobody stored. */
    ASSERT_TRUE(fs_write_file(config, "default = gcc@12.3.0\n"));

    char id[PREFERENCE_VALUE_MAX];
    EXPECT_FALSE(preference_default_get(id, sizeof id));

    fixture_teardown(&fixture);
}

MOLTEST(preference_reads_the_registry_it_was_pointed_at) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(config_path(&fixture, config, sizeof config));
    ASSERT_TRUE(fs_write_file(config, "registry = \"https://mine.invalid\"\n"));

    char url[PREFERENCE_URL_MAX];
    ASSERT_TRUE(preference_registry_get(url, sizeof url));
    EXPECT_STREQ("https://mine.invalid", url);

    fixture_teardown(&fixture);
}

MOLTEST(preference_keeps_the_registry_when_the_default_changes) {
    preference_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(config_path(&fixture, config, sizeof config));
    ASSERT_TRUE(fs_write_file(config, "registry = \"https://mine.invalid\"\n"));

    /* Two keys, one file. Recording a default must not quietly redirect every
       future install by dropping the other. */
    ASSERT_TRUE(preference_default_set("gcc@12.3.0"));

    char url[PREFERENCE_URL_MAX];
    ASSERT_TRUE(preference_registry_get(url, sizeof url));
    EXPECT_STREQ("https://mine.invalid", url);

    char id[PREFERENCE_VALUE_MAX];
    ASSERT_TRUE(preference_default_get(id, sizeof id));
    EXPECT_STREQ("gcc@12.3.0", id);

    fixture_teardown(&fixture);
}
