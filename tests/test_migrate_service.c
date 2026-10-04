#include <moltest.h>

#include <pickup/detect/recipe.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/migrate_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/preference_service.h>
#include <pickup/services/process_service.h>

#include "user_dirs_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

/*
 * An upgrade, replayed in a temporary home: a ~/.pickup laid out the way every
 * earlier version left it, and the platform directories this one writes to.
 * Nothing here may lose what was installed, and nothing may leave a toolchain
 * that stops building because it was moved.
 */

/* The legacy directory of the fixture, and one path inside it. */
static bool legacy_path(char *out, size_t out_size, const char *relative) {
    char legacy[PICKUP_PATHS_MAX];
    if (!paths_legacy_home(legacy, sizeof legacy))
        return false;
    if (relative == NULL)
        return fs_format_path(out, out_size, "%s", legacy);
    return fs_format_path(out, out_size, "%s/%s", legacy, relative);
}

/* Write `content` at `path`, creating what is above it. */
static bool plant(const char *path, const char *content) {
    char parent[PICKUP_PATHS_MAX];
    if (!fs_format_path(parent, sizeof parent, "%s", path))
        return false;
    char *slash = strrchr(parent, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return fs_make_dirs(parent) && fs_write_file(path, content);
}

static bool plant_legacy(const char *relative, const char *content) {
    char path[PICKUP_PATHS_MAX];
    return legacy_path(path, sizeof path, relative) && plant(path, content);
}

/* What an older version would have left: one toolchain, one tool, a
   preference, and the disposable rest. */
static bool plant_a_used_legacy_home(void) {
    return plant_legacy("toolchains/clang-1.0.0-x86_64-unknown-linux-gnu/bin/clang", "driver") &&
           plant_legacy("toolchains/clang-1.0.0-x86_64-unknown-linux-gnu/.pickup-artifact",
                        "name = \"clang\"\n") &&
           plant_legacy("tools/clang-format-19.1.6/bin/clang-format", "formatter") &&
           plant_legacy("config.toml", "default = \"clang@1.0.0\"\n") &&
           plant_legacy("cache/toolchains", "inventory") &&
           plant_legacy("cache/toolchain-index.json", "{}") &&
           plant_legacy("downloads/clang.tar.zst", "half an archive");
}

/* The same path under the data directory. */
static bool data_path(char *out, size_t out_size, const char *relative) {
    char data[PICKUP_PATHS_MAX];
    return paths_data(data, sizeof data) &&
           fs_format_path(out, out_size, "%s/%s", data, relative);
}

static bool data_has(const char *relative) {
    char path[PICKUP_PATHS_MAX];
    return data_path(path, sizeof path, relative) && fs_path_exists(path);
}

static bool legacy_has(const char *relative) {
    char path[PICKUP_PATHS_MAX];
    return legacy_path(path, sizeof path, relative) && fs_path_exists(path);
}

static bool report_names(const migrate_report *report, const char *name) {
    const size_t named = report->left_count < MIGRATE_LEFT_MAX ? report->left_count
                                                               : MIGRATE_LEFT_MAX;
    for (size_t i = 0; i < named; i++) {
        if (strcmp(report->left[i], name) == 0)
            return true;
    }
    return false;
}

MOLTEST(migrate_does_nothing_without_a_legacy_home) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));

    const migrate_report report = migrate_legacy_home();
    EXPECT_FALSE(report.attempted);

    /* And creates nothing: a first run on a new machine is not a reason to
       make directories nobody has asked for yet. */
    char data[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_data(data, sizeof data));
    EXPECT_FALSE(fs_path_exists(data));

    user_dirs_teardown(&fixture);
}

MOLTEST(migrate_leaves_a_relocated_home_alone) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));
    ASSERT_TRUE(plant_a_used_legacy_home());
    ASSERT_TRUE(user_dirs_point(&fixture, PICKUP_HOME_ENV, "/relocated"));

    /* Whoever set PICKUP_HOME chose where everything lives; reaching into
       ~/.pickup behind their back would be moving files they did not ask
       about — and in the test suite, files of the person running it. */
    const migrate_report report = migrate_legacy_home();
    EXPECT_FALSE(report.attempted);
    EXPECT_TRUE(legacy_has("toolchains/clang-1.0.0-x86_64-unknown-linux-gnu/bin/clang"));

    user_dirs_teardown(&fixture);
}

MOLTEST(migrate_moves_toolchains_tools_and_preferences) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));
    ASSERT_TRUE(plant_a_used_legacy_home());

    const migrate_report report = migrate_legacy_home();
    ASSERT_TRUE(report.attempted);
    EXPECT_EQ(1, (int)report.toolchains_moved);
    EXPECT_EQ(1, (int)report.tools_moved);
    EXPECT_TRUE(report.preferences_moved);
    EXPECT_EQ(0, (int)report.left_count);

    EXPECT_TRUE(data_has("toolchains/clang-1.0.0-x86_64-unknown-linux-gnu/bin/clang"));
    /* The install receipt is relative, and travels with the directory. */
    EXPECT_TRUE(data_has("toolchains/clang-1.0.0-x86_64-unknown-linux-gnu/.pickup-artifact"));
    EXPECT_TRUE(data_has("tools/clang-format-19.1.6/bin/clang-format"));

    /* The preference reads back through the service that owns it. */
    char id[PREFERENCE_VALUE_MAX];
    ASSERT_TRUE(preference_default_get(id, sizeof id));
    EXPECT_STREQ("clang@1.0.0", id);

    /* Everything moved, so nothing is left to keep the old directory for —
       the cache included, which is rebuilt rather than carried. */
    EXPECT_TRUE(report.legacy_removed);
    EXPECT_FALSE(legacy_has(NULL));
    char cache[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    char inventory[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(inventory, sizeof inventory, "%s/toolchains", cache));
    EXPECT_FALSE(fs_path_exists(inventory));

    user_dirs_teardown(&fixture);
}

MOLTEST(migrate_happens_once) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));
    ASSERT_TRUE(plant_a_used_legacy_home());

    const migrate_report first = migrate_legacy_home();
    ASSERT_TRUE(first.attempted);

    /* Run on every invocation, so the second has to be nothing at all. */
    const migrate_report second = migrate_legacy_home();
    EXPECT_FALSE(second.attempted);
    EXPECT_TRUE(data_has("toolchains/clang-1.0.0-x86_64-unknown-linux-gnu/bin/clang"));
    EXPECT_TRUE(data_has("tools/clang-format-19.1.6/bin/clang-format"));

    user_dirs_teardown(&fixture);
}

MOLTEST(migrate_keeps_the_legacy_home_when_something_cannot_move) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));
    ASSERT_TRUE(plant_a_used_legacy_home());
    /* A name already taken at the destination, and a file nobody told the
       migration about. Neither is Pickup's to overwrite or to throw away. */
    char taken[PICKUP_PATHS_MAX];
    ASSERT_TRUE(data_path(taken, sizeof taken, "tools/clang-format-19.1.6/bin/clang-format"));
    ASSERT_TRUE(plant(taken, "the one already there"));
    ASSERT_TRUE(plant_legacy("notes.txt", "mine"));

    const migrate_report report = migrate_legacy_home();
    ASSERT_TRUE(report.attempted);
    EXPECT_EQ(1, (int)report.toolchains_moved);
    EXPECT_EQ(0, (int)report.tools_moved);
    EXPECT_EQ(2, (int)report.left_count);
    EXPECT_TRUE(report_names(&report, "tools/clang-format-19.1.6"));
    EXPECT_TRUE(report_names(&report, "notes.txt"));

    /* What moved, moved; what did not is exactly where it was. */
    EXPECT_TRUE(data_has("toolchains/clang-1.0.0-x86_64-unknown-linux-gnu/bin/clang"));
    EXPECT_FALSE(report.legacy_removed);
    EXPECT_TRUE(legacy_has("tools/clang-format-19.1.6/bin/clang-format"));
    EXPECT_TRUE(legacy_has("notes.txt"));
    char *kept = fs_read_file(taken);
    ASSERT_TRUE(kept != NULL);
    EXPECT_STREQ("the one already there", kept);
    free(kept);

    /* Said once, not on every run: the note left behind is what stops it. */
    EXPECT_TRUE(legacy_has(MIGRATE_MARKER_FILENAME));
    const migrate_report again = migrate_legacy_home();
    EXPECT_FALSE(again.attempted);

    /* And removing the note asks for another try. */
    char marker[PICKUP_PATHS_MAX];
    ASSERT_TRUE(legacy_path(marker, sizeof marker, MIGRATE_MARKER_FILENAME));
    ASSERT_TRUE(remove(marker) == 0);
    const migrate_report retried = migrate_legacy_home();
    EXPECT_TRUE(retried.attempted);

    user_dirs_teardown(&fixture);
}

MOLTEST(migrate_does_not_overwrite_preferences_already_in_place) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));
    ASSERT_TRUE(plant_legacy("config.toml", "default = \"old@1\"\n"));
    ASSERT_TRUE(preference_default_set("new@2"));

    const migrate_report report = migrate_legacy_home();
    ASSERT_TRUE(report.attempted);
    EXPECT_FALSE(report.preferences_moved);
    EXPECT_TRUE(report_names(&report, "config.toml"));

    char id[PREFERENCE_VALUE_MAX];
    ASSERT_TRUE(preference_default_get(id, sizeof id));
    EXPECT_STREQ("new@2", id);
    EXPECT_TRUE(legacy_has("config.toml"));

    user_dirs_teardown(&fixture);
}

/* --- a toolchain that names its own location --- */

#define MOVED_TOOLCHAIN "clang-1.0.0-x86_64-unknown-linux-gnu"
#define MOVED_GCC "gcc-12.3.0-x86_64-conda-linux-gnu"

/* A .cfg as `install` writes one, through the same function: its directories
   are absolute, and every one of them is under the legacy home. */
static bool configure_in_legacy(const char *driver_relative, char *driver, size_t driver_size) {
    char toolchain[PICKUP_PATHS_MAX];
    if (!legacy_path(toolchain, sizeof toolchain, "toolchains/" MOVED_TOOLCHAIN))
        return false;
    char gcc[PICKUP_PATHS_MAX];
    if (!legacy_path(gcc, sizeof gcc, "toolchains/" MOVED_GCC))
        return false;
    if (!fs_format_path(driver, driver_size, "%s/%s", toolchain, driver_relative) ||
        !plant(driver, "driver"))
        return false;

    link_recipe recipe = {0};
    recipe.usable = true;
    (void)fs_format_path(recipe.link_flags[recipe.link_count++], RECIPE_FLAG_MAX, "-I%s/include",
                         toolchain);
    (void)fs_format_path(recipe.link_flags[recipe.link_count++], RECIPE_FLAG_MAX,
                         "-Wl,-rpath,%s/lib", toolchain);
    (void)fs_format_path(recipe.link_flags[recipe.link_count++], RECIPE_FLAG_MAX,
                         "--gcc-install-dir=%s/lib/gcc/x86_64-conda-linux-gnu/12.3.0", gcc);
    recipe.compile_count = recipe.link_count;

    char include[PICKUP_PATHS_MAX];
    char lib[PICKUP_PATHS_MAX];
    char gcc_dir[PICKUP_PATHS_MAX];
    return fs_format_path(include, sizeof include, "%s/include/own.h", toolchain) &&
           plant(include, "#define OWN 42\n") &&
           fs_format_path(lib, sizeof lib, "%s/lib", toolchain) && fs_make_dirs(lib) &&
           fs_format_path(gcc_dir, sizeof gcc_dir, "%s/lib/gcc/x86_64-conda-linux-gnu/12.3.0",
                          gcc) &&
           fs_make_dirs(gcc_dir) && recipe_write_config(driver, &recipe);
}

static char *moved_config(const char *driver_relative) {
    char path[PICKUP_PATHS_MAX];
    if (!data_path(path, sizeof path, "toolchains/" MOVED_TOOLCHAIN) ||
        !fs_format_path(path + strlen(path), sizeof path - strlen(path), "/%s%s", driver_relative,
                        RECIPE_CONFIG_SUFFIX))
        return NULL;
    return fs_read_file(path);
}

MOLTEST(migrate_points_a_moved_configuration_at_the_new_location) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));

    char driver[PICKUP_PATHS_MAX];
    ASSERT_TRUE(configure_in_legacy("bin/clang", driver, sizeof driver));
    char legacy[PICKUP_PATHS_MAX];
    ASSERT_TRUE(legacy_path(legacy, sizeof legacy, NULL));

    const migrate_report report = migrate_legacy_home();
    ASSERT_TRUE(report.attempted);
    EXPECT_EQ(2, (int)report.toolchains_moved);

    char *config = moved_config("bin/clang");
    ASSERT_TRUE(config != NULL);

    char toolchains[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_toolchains(toolchains, sizeof toolchains));
    char expected[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(expected, sizeof expected, "-Wl,-rpath,%s/" MOVED_TOOLCHAIN "/lib\n", toolchains));
    EXPECT_TRUE(strstr(config, expected) != NULL);
    ASSERT_TRUE(fs_format_path(expected, sizeof expected, "-I%s/" MOVED_TOOLCHAIN "/include\n", toolchains));
    EXPECT_TRUE(strstr(config, expected) != NULL);
    /* The GCC it stands on moved too, and is named where it went. */
    ASSERT_TRUE(fs_format_path(expected, sizeof expected, "--gcc-install-dir=%s/" MOVED_GCC, toolchains));
    EXPECT_TRUE(strstr(config, expected) != NULL);
    /* Nothing still points at the directory that no longer exists. */
    EXPECT_TRUE(strstr(config, legacy) == NULL);
    free(config);

    user_dirs_teardown(&fixture);
}

/*
 * A driver that, like Clang, reads the .cfg beside it on every run, and fails
 * the way a real one does when a directory named there is not where the file
 * says: the header is not found, the runtime is not found.
 */
MOLTEST_FAKE(driver_reading_its_config) {
    (void)argc;
    (void)argv;
    const char *config_file = moltest_fake_setting("config");
    char *config = config_file != NULL ? fs_read_file(config_file) : NULL;
    if (config == NULL)
        return 2;

    int status = 0;
    for (char *line = strtok(config, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        const char *directory = NULL;
        if (strncmp(line, "-I", 2) == 0)
            directory = line + 2;
        else if (strncmp(line, "-Wl,-rpath,", 11) == 0)
            directory = line + 11;
        else if (strncmp(line, "--gcc-install-dir=", 18) == 0)
            directory = line + 18;
        if (directory != NULL && !fs_is_dir(directory)) {
            fprintf(stderr, "no such directory: %s\n", directory);
            status = 1;
        }
    }
    free(config);
    return status;
}

MOLTEST(migrate_leaves_a_configured_toolchain_that_still_finds_its_directories) {
    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));

    char driver[PICKUP_PATHS_MAX];
    ASSERT_TRUE(configure_in_legacy("bin/clang", driver, sizeof driver));

    /* Planted where the migration will put it, reading the file the migration
       will have rewritten: the fake has to be told where its config is, since
       a copied test binary does not know its own name on every platform. */
    char moved_driver[PICKUP_PATHS_MAX];
    char moved_cfg[PICKUP_PATHS_MAX];
    ASSERT_TRUE(data_path(moved_driver, sizeof moved_driver,
                          "toolchains/" MOVED_TOOLCHAIN "/bin/clang"));
    ASSERT_TRUE(fs_format_path(moved_cfg, sizeof moved_cfg, "%s%s", moved_driver,
                               RECIPE_CONFIG_SUFFIX));

    const migrate_report report = migrate_legacy_home();
    ASSERT_TRUE(report.attempted);
    ASSERT_TRUE(report.legacy_removed);

    char spec[PICKUP_PATHS_MAX + 64];
    snprintf(spec, sizeof spec, "set config %s\nbehave driver_reading_its_config\n", moved_cfg);
    char made[PICKUP_PATHS_MAX];
    ASSERT_TRUE(moltest_fake_program(moved_driver, spec, made, sizeof made));

    const char *argv[] = {made, "--version", NULL};
    const process_result result = process_try(argv, NULL);
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(0, result.exit_code);

    /* And it is still recognised as one Pickup installed, so `uninstall` can
       remove it from where it now lives. */
    char owner[PICKUP_PATHS_MAX];
    char resolved[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_real_path(made, resolved, sizeof resolved));
    EXPECT_TRUE(paths_owning_toolchain(resolved, owner, sizeof owner));

    user_dirs_teardown(&fixture);
}

#ifndef _WIN32
/* The system's C compiler, when there is one to wrap. */
static bool have_cc(void) {
    const char *argv[] = {"cc", "--version", NULL};
    const process_result result = process_try(argv, NULL);
    return result.completed && result.exit_code == 0;
}

/*
 * The same, with a real compiler: a driver that, like Clang, applies the .cfg
 * beside it, a header the toolchain carries in its own include/, and a program
 * that does not build unless the file names where that header now is.
 */
MOLTEST(migrate_leaves_a_toolchain_that_still_compiles) {
    if (!have_cc())
        SKIP("a system cc is needed to stand behind the toolchain's driver");

    user_dirs_fixture fixture;
    ASSERT_TRUE(user_dirs_setup(&fixture, "pickup_migrate"));

    char driver[PICKUP_PATHS_MAX];
    ASSERT_TRUE(configure_in_legacy("bin/clang", driver, sizeof driver));
    ASSERT_TRUE(plant(driver, "#!/bin/sh\n"
                              "set -- $(grep -v '^#' \"$0.cfg\" | grep -v -e '-rpath' "
                              "-e '--gcc-install-dir') \"$@\"\n"
                              "exec cc \"$@\"\n"));
    ASSERT_TRUE(chmod(driver, 0755) == 0);

    char source[PICKUP_PATHS_MAX];
    char program[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(source, sizeof source, "%s/main.c", fixture.root));
    ASSERT_TRUE(fs_format_path(program, sizeof program, "%s/main", fixture.root));
    ASSERT_TRUE(plant(source, "#include <own.h>\nint main(void) { return OWN - 42; }\n"));

    /* Builds where it is, before anything moves: the test can tell. */
    const char *before[] = {driver, source, "-o", program, NULL};
    process_result built = process_try(before, NULL);
    ASSERT_TRUE(built.completed);
    ASSERT_EQ(0, built.exit_code);

    const migrate_report report = migrate_legacy_home();
    ASSERT_TRUE(report.attempted);
    ASSERT_TRUE(report.legacy_removed);

    char moved[PICKUP_PATHS_MAX];
    ASSERT_TRUE(data_path(moved, sizeof moved, "toolchains/" MOVED_TOOLCHAIN "/bin/clang"));
    (void)remove(program);
    const char *after[] = {moved, source, "-o", program, NULL};
    built = process_try(after, NULL);
    ASSERT_TRUE(built.completed);
    EXPECT_EQ(0, built.exit_code);

    const char *run[] = {program, NULL};
    const process_result ran = process_try(run, NULL);
    ASSERT_TRUE(ran.completed);
    EXPECT_EQ(0, ran.exit_code);

    user_dirs_teardown(&fixture);
}
#endif

MOLTEST(migrate_announces_what_moved_and_what_did_not) {
    migrate_report report = {0};
    report.attempted = true;
    report.toolchains_moved = 2;
    report.tools_moved = 1;
    report.preferences_moved = true;
    report.left_count = 1;
    snprintf(report.left[0], sizeof report.left[0], "notes.txt");
    snprintf(report.legacy, sizeof report.legacy, "/h/.pickup");
    snprintf(report.data, sizeof report.data, "/h/.local/share/pickup");
    snprintf(report.config, sizeof report.config, "/h/.config/pickup");

    char path[PICKUP_PATHS_MAX];
    ASSERT_TRUE(moltest_temp_file("pickup_announce", path, sizeof path));
    FILE *stream = fopen(path, "w");
    ASSERT_TRUE(stream != NULL);
    migrate_announce(&report, stream);
    fclose(stream);

    char *said = fs_read_file(path);
    ASSERT_TRUE(said != NULL);
    /* What moved, from where, to where — and what is still in the old place. */
    EXPECT_TRUE(strstr(said, "2 toolchains") != NULL);
    EXPECT_TRUE(strstr(said, "1 tool") != NULL);
    EXPECT_TRUE(strstr(said, "/h/.pickup") != NULL);
    EXPECT_TRUE(strstr(said, "/h/.local/share/pickup") != NULL);
    EXPECT_TRUE(strstr(said, "/h/.config/pickup") != NULL);
    EXPECT_TRUE(strstr(said, "notes.txt") != NULL);

    /* One or two lines, never a wall of text on every upgrade. */
    size_t lines = 0;
    for (const char *at = said; *at != '\0'; at++)
        lines += *at == '\n';
    EXPECT_TRUE(lines >= 1 && lines <= 2);
    free(said);
    (void)remove(path);

    /* And nothing at all when nothing was attempted. */
    ASSERT_TRUE(moltest_temp_file("pickup_announce", path, sizeof path));
    stream = fopen(path, "w");
    ASSERT_TRUE(stream != NULL);
    const migrate_report nothing = {0};
    migrate_announce(&nothing, stream);
    fclose(stream);
    long long size = -1;
    ASSERT_TRUE(fs_file_size(path, &size));
    EXPECT_EQ(0, (int)size);
    (void)remove(path);
}
