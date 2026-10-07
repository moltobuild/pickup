#include <moltest.h>

#include <pickup/commands/uninstall_command.h>
#include <pickup/exit_code.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>

#include <stdio.h>
#include <stdlib.h>

/*
 * Removing a tool pickup installed.
 *
 * Tools live apart from toolchains, under <home>/tools/<name>-<version>, and
 * several versions of one side by side is what an upgrade leaves behind. So the
 * rule is the one toolchains already follow: a name that matches one thing
 * removes it, and a name that matches several removes nothing and lists them.
 * `assume_yes` stands in for the person answering the prompt.
 */

typedef struct {
    char root[64];
    char previous_home[4096];
    bool had_home;
} uninstall_fixture;

static bool fixture_setup(uninstall_fixture *fixture) {
    if (!moltest_temp_dir("pickup_uninstall", fixture->root, sizeof fixture->root))
        return false;
    const char *home = getenv(PICKUP_HOME_ENV);
    fixture->had_home = home != NULL;
    if (home != NULL)
        snprintf(fixture->previous_home, sizeof fixture->previous_home, "%s", home);
    return setenv(PICKUP_HOME_ENV, fixture->root, 1) == 0;
}

static void fixture_teardown(uninstall_fixture *fixture) {
    if (fixture->had_home)
        (void)setenv(PICKUP_HOME_ENV, fixture->previous_home, 1);
    else
        (void)unsetenv(PICKUP_HOME_ENV);
    (void)fs_remove_tree(fixture->root);
}

/* <home>/tools/<name>-<version>/bin/<name>, the layout install leaves. */
static bool plant(const char *name, const char *version) {
    char bin[PICKUP_PATHS_MAX];
    char tools[PICKUP_PATHS_MAX];
    if (!paths_tools(tools, sizeof tools) ||
        !fs_format_path(bin, sizeof bin, "%s/%s-%s/bin", tools, name, version) ||
        !fs_make_dirs(bin))
        return false;
    char path[PICKUP_PATHS_MAX];
    return fs_format_path(path, sizeof path, "%s/%s", bin, name) && fs_write_file(path, "");
}

static bool installed(const char *name, const char *version) {
    char tools[PICKUP_PATHS_MAX];
    char directory[PICKUP_PATHS_MAX];
    return paths_tools(tools, sizeof tools) &&
           fs_format_path(directory, sizeof directory, "%s/%s-%s", tools, name, version) &&
           fs_is_dir(directory);
}

DESCRIBE(uninstall_removes_the_tool_version_it_names) {
    uninstall_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    ASSERT_TRUE(plant("clang-tidy", "19.1.6"));
    ASSERT_TRUE(plant("clang-tidy", "21.1.8"));

    EXPECT_EQ(exit_ok, uninstall_command_run("clang-tidy@19.1.6", true));
    EXPECT_FALSE(installed("clang-tidy", "19.1.6"));
    EXPECT_TRUE(installed("clang-tidy", "21.1.8"));

    fixture_teardown(&fixture);
}

/* A bare name is enough when there is only one to mean. */
DESCRIBE(uninstall_takes_a_bare_tool_name_when_one_version_is_installed) {
    uninstall_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    ASSERT_TRUE(plant("clang-format", "21.1.8"));

    EXPECT_EQ(exit_ok, uninstall_command_run("clang-format", true));
    EXPECT_FALSE(installed("clang-format", "21.1.8"));

    fixture_teardown(&fixture);
}

/* And refused when there are several: removing the one the user did not
   picture is the failure the toolchain side already guards against. */
DESCRIBE(uninstall_refuses_a_bare_tool_name_that_means_several_versions) {
    uninstall_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    ASSERT_TRUE(plant("clang-tidy", "19.1.6"));
    ASSERT_TRUE(plant("clang-tidy", "21.1.8"));

    EXPECT_EQ(exit_usage_error, uninstall_command_run("clang-tidy", true));
    EXPECT_TRUE(installed("clang-tidy", "19.1.6"));
    EXPECT_TRUE(installed("clang-tidy", "21.1.8"));

    fixture_teardown(&fixture);
}

/* A version that is not installed is an answer, not a breakage — and a name
   that only begins like another tool's is not that tool. */
DESCRIBE(uninstall_says_when_that_tool_version_is_not_installed) {
    uninstall_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    ASSERT_TRUE(plant("clang-tidy", "21.1.8"));
    ASSERT_TRUE(plant("clang-tidy-extra", "1.0.0"));

    EXPECT_EQ(exit_no_match, uninstall_command_run("clang-tidy@9.9.9", true));
    EXPECT_TRUE(installed("clang-tidy", "21.1.8"));

    /* "clang-tidy" means clang-tidy-21.1.8, not clang-tidy-extra-1.0.0. */
    EXPECT_EQ(exit_ok, uninstall_command_run("clang-tidy", true));
    EXPECT_TRUE(installed("clang-tidy-extra", "1.0.0"));

    fixture_teardown(&fixture);
}
