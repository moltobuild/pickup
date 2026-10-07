#include <moltest.h>

#include <pickup/commands/uninstall_command.h>
#include <pickup/detect/tools.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/inventory_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/preference_service.h>

#include "user_dirs_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

/*
 * The commands, run with nothing relocated: what they read and write has to be
 * in the platform directories, and none of it in the legacy ~/.pickup.
 */

static bool legacy_exists(void) {
    char legacy[PICKUP_PATHS_MAX];
    return paths_legacy_home(legacy, sizeof legacy) && fs_path_exists(legacy);
}

/* `path` is somewhere under `root`. */
static bool inside(const char *path, const char *root) {
    const size_t length = strlen(root);
    return strncmp(path, root, length) == 0 && path[length] == '/';
}

DESCRIBE(default_keeps_the_preference_in_the_config_directory) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_user_dirs"));

    ASSERT_TRUE(preference_default_set("clang@22.1.8"));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_config(config, sizeof config));
    char file[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(file, sizeof file, "%s/config.toml", config));
    EXPECT_TRUE(fs_path_exists(file));
    EXPECT_FALSE(legacy_exists());

    /* And clearing it goes through the same file. */
    ASSERT_TRUE(preference_default_clear());
    EXPECT_FALSE(fs_path_exists(file));

    user_dirs_teardown(&fixture);
}

DESCRIBE(tools_finds_what_was_installed_into_the_data_directory) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_user_dirs"));

    /* PATH holds nothing, so whatever is found came from the data directory. */
    char empty[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(empty, sizeof empty, "%s/empty", fixture.root));
    ASSERT_TRUE(fs_make_dirs(empty));
    char previous_path[8192];
    const char *path = getenv("PATH");
    snprintf(previous_path, sizeof previous_path, "%s", path != NULL ? path : "");
    ASSERT_TRUE(setenv("PATH", empty, 1) == 0);

    char tools[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_tools(tools, sizeof tools));
    char binary[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(binary, sizeof binary, "%s/clang-format-19.1.6/bin", tools));
    ASSERT_TRUE(fs_make_dirs(binary));
    ASSERT_TRUE(fs_format_path(binary + strlen(binary), sizeof binary - strlen(binary),
                               "/clang-format"));
    ASSERT_TRUE(moltest_fake_program(binary, "out clang-format version 19.1.6\nexit 0\n", NULL, 0));

    dev_tool found[TOOLS_MAX];
    const size_t count = tools_discover(found, TOOLS_MAX);
    const dev_tool *formatter = NULL;
    for (size_t i = 0; i < count; i++) {
        if (found[i].kind == tool_formatter)
            formatter = &found[i];
    }
    (void)setenv("PATH", previous_path, 1);

    ASSERT_TRUE(formatter != NULL);
    EXPECT_EQ(toolchain_source_pickup, formatter->source);
    EXPECT_TRUE(inside(formatter->path, tools));

    user_dirs_teardown(&fixture);
}

#ifndef _WIN32
/*
 * `list` reports a toolchain under the data directory as one Pickup installed,
 * and `uninstall` removes it from there. A wrapper around the system's own
 * compiler, so that what is probed really compiles.
 */
DESCRIBE(list_and_uninstall_use_the_data_directory) {
    if (access("/usr/bin/cc", X_OK) != 0)
        SKIP("/usr/bin/cc is needed to stand behind the installed driver");

    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_user_dirs"));

    char empty[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(empty, sizeof empty, "%s/empty", fixture.root));
    ASSERT_TRUE(fs_make_dirs(empty));
    char previous_path[8192];
    const char *path = getenv("PATH");
    snprintf(previous_path, sizeof previous_path, "%s", path != NULL ? path : "");
    ASSERT_TRUE(setenv("PATH", empty, 1) == 0);

    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));
    char directory[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(directory, sizeof directory, "%s/cc-1.0.0-host", toolchains));
    char driver[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(driver, sizeof driver, "%s/bin", directory));
    ASSERT_TRUE(fs_make_dirs(driver));
    ASSERT_TRUE(fs_format_path(driver + strlen(driver), sizeof driver - strlen(driver), "/gcc"));
    ASSERT_TRUE(fs_write_file(driver, "#!/bin/sh\nexec /usr/bin/cc \"$@\"\n"));
    ASSERT_TRUE(chmod(driver, 0755) == 0);

    inventory list;
    ASSERT_TRUE(inventory_load(&list, true));
    const toolchain *found = NULL;
    for (size_t i = 0; i < list.count; i++) {
        if (strstr(list.items[i].path, "cc-1.0.0-host") != NULL)
            found = &list.items[i];
    }
    ASSERT_TRUE(found != NULL);
    EXPECT_EQ(toolchain_source_pickup, found->source);
    char name[PICKUP_PATHS_MAX];
    snprintf(name, sizeof name, "%s", found->path);
    inventory_free(&list);

    /* The inventory it was served from is cached in the cache directory. */
    char cache[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    char cached[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(cached, sizeof cached, "%s/toolchains", cache));
    EXPECT_TRUE(fs_path_exists(cached));

    EXPECT_EQ(0, uninstall_command_run(name, true));
    EXPECT_FALSE(fs_path_exists(directory));
    EXPECT_FALSE(legacy_exists());

    (void)setenv("PATH", previous_path, 1);
    user_dirs_teardown(&fixture);
}
#endif
