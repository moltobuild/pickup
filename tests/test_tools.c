#include <moltest.h>

#include <pickup/detect/tools.h>
#include <pickup/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Finding the tools a project is worked on with.
 *
 * The claim worth testing is that a name on a filesystem is not a tool. A file
 * called `clang-format` that cannot be run, or that does not answer when
 * asked, must not be reported as one — for the same reason a compiler is made
 * to compile rather than believed on the strength of its filename.
 */

typedef struct {
    char root[64];
    char previous_path[4096];
    char previous_home[PICKUP_PATHS_MAX];
    bool had_home;
} tools_fixture;

/*
 * Both places tools are looked for have to be pointed at the fixture: PATH,
 * and the pickup home.
 *
 * Leaving the home alone makes the test depend on what the person running it
 * happens to have installed — a real clang-format under ~/.pickup/tools turns
 * "finds nothing on a bare machine" into a failure that says nothing about the
 * code.
 */
static bool fixture_setup(tools_fixture *fixture) {
    if (!moltest_temp_dir("pickup_tools", fixture->root, sizeof fixture->root))
        return false;

    const char *path = getenv("PATH");
    snprintf(fixture->previous_path, sizeof fixture->previous_path, "%s",
             path != NULL ? path : "");

    const char *home = getenv(PICKUP_HOME_ENV);
    fixture->had_home = home != NULL;
    if (home != NULL)
        snprintf(fixture->previous_home, sizeof fixture->previous_home, "%s", home);

    return setenv("PATH", fixture->root, 1) == 0
        && setenv(PICKUP_HOME_ENV, fixture->root, 1) == 0;
}

static void fixture_teardown(tools_fixture *fixture) {
    (void)setenv("PATH", fixture->previous_path, 1);
    if (fixture->had_home)
        (void)setenv(PICKUP_HOME_ENV, fixture->previous_home, 1);
    else
        (void)unsetenv(PICKUP_HOME_ENV);
    (void)fs_remove_tree(fixture->root);
}

/* Put a runnable stand-in for `name` in the fixture. */
static bool plant_working(const tools_fixture *fixture, const char *name,
                          const char *version) {
    char path[PICKUP_PATHS_MAX];
    if (!fs_format_path(path, sizeof path, "%s/%s", fixture->root, name))
        return false;

    /* One `out` per line: the spec is read line by line, so a banner that
       spans several is several directives rather than one with newlines in
       it. */
    char spec[512];
    size_t at = 0;
    for (const char *line = version; line != NULL && at < sizeof spec;) {
        const char *end = strchr(line, '\n');
        const int width = end != NULL ? (int)(end - line) : (int)strlen(line);
        const int written = snprintf(spec + at, sizeof spec - at, "out %.*s\n", width, line);
        if (written < 0 || (size_t)written >= sizeof spec - at)
            return false;
        at += (size_t)written;
        line = end != NULL ? end + 1 : NULL;
    }
    if (at + sizeof "exit 0\n" > sizeof spec)
        return false;
    snprintf(spec + at, sizeof spec - at, "exit 0\n");
    return moltest_fake_program(path, spec, NULL, 0);
}

/* And one that is there and does not work. */
static bool plant_broken(const tools_fixture *fixture, const char *name) {
    char path[PICKUP_PATHS_MAX];
    if (!fs_format_path(path, sizeof path, "%s/%s", fixture->root, name))
        return false;
    return moltest_fake_program(path, "exit 1\n", NULL, 0);
}

static const dev_tool *of_kind(const dev_tool *found, size_t count, tool_kind kind) {
    for (size_t i = 0; i < count; i++) {
        if (found[i].kind == kind)
            return &found[i];
    }
    return NULL;
}

MOLTEST(tools_finds_a_formatter_and_reads_its_version) {
    tools_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    ASSERT_TRUE(plant_working(&fixture, "clang-format", "clang-format version 22.1.8"));

    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);

    const dev_tool *formatter = of_kind(found, count, tool_formatter);
    ASSERT_TRUE(formatter != NULL);
    EXPECT_STREQ("clang-format", formatter->name);
    /* What it said when asked, not what was assumed from its name. */
    EXPECT_STREQ("clang-format version 22.1.8", formatter->version);

    fixture_teardown(&fixture);
}

MOLTEST(tools_does_not_count_something_that_only_has_the_right_name) {
    tools_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    /* Executable, correctly named, and it does not answer. Reporting it would
       be claiming a capability nobody checked. */
    ASSERT_TRUE(plant_broken(&fixture, "clang-format"));

    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);
    EXPECT_NULL(of_kind(found, count, tool_formatter));

    fixture_teardown(&fixture);
}

MOLTEST(tools_finds_nothing_on_a_bare_machine) {
    tools_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);
    /* An empty answer is a result. It is what makes doctor say the formatter
       is missing rather than say nothing at all. */
    EXPECT_EQ(0, (int)count);

    fixture_teardown(&fixture);
}

MOLTEST(tools_takes_either_linter) {
    tools_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    /* cppcheck predates clang-tidy and is still what many projects use, so a
       machine with it is not a machine without a linter. */
    ASSERT_TRUE(plant_working(&fixture, "cppcheck", "Cppcheck 2.21.0"));

    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);

    const dev_tool *linter = of_kind(found, count, tool_linter);
    ASSERT_TRUE(linter != NULL);
    EXPECT_STREQ("cppcheck", linter->name);

    fixture_teardown(&fixture);
}

MOLTEST(tools_knows_which_names_it_looks_for) {
    tool_kind kind;
    ASSERT_TRUE(tools_kind_of("clang-format", &kind));
    EXPECT_EQ(tool_formatter, kind);
    ASSERT_TRUE(tools_kind_of("clang-tidy", &kind));
    EXPECT_EQ(tool_linter, kind);
    ASSERT_TRUE(tools_kind_of("cppcheck", &kind));
    EXPECT_EQ(tool_linter, kind);
    /* Nothing in a build runs it, and `install clangd` still has to work: it
       is the tool that has to match the compiler it indexes for. */
    ASSERT_TRUE(tools_kind_of("clangd", &kind));
    EXPECT_EQ(tool_language_server, kind);

    /* A toolchain is not a tool, and install has to tell them apart. */
    EXPECT_FALSE(tools_kind_of("clang", &kind));
    EXPECT_FALSE(tools_kind_of("gcc", &kind));
    EXPECT_FALSE(tools_kind_of(NULL, &kind));
}

MOLTEST(tools_names_every_kind_and_what_provides_it) {
    const tool_kind kinds[] = { tool_formatter, tool_linter, tool_language_server };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        const char *name = tool_kind_name(kinds[i]);
        const char *package = tool_kind_package(kinds[i]);
        ASSERT_TRUE(name != NULL && package != NULL);
        EXPECT_TRUE(name[0] != '\0' && package[0] != '\0');
        /* Whatever is named has to be something install can actually fetch. */
        tool_kind kind;
        EXPECT_TRUE(tools_kind_of(package, &kind));
    }
}

MOLTEST(tools_reads_a_version_that_is_not_on_the_first_line) {
    tools_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    /* clang-tidy opens with a banner and puts the version underneath. Taking
       the first line would report the tool as "LLVM". */
    ASSERT_TRUE(plant_working(&fixture, "clang-tidy",
                              "LLVM (http://llvm.org/):\n  LLVM version 22.1.8\n"
                              "  Optimized build."));

    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);

    const dev_tool *linter = of_kind(found, count, tool_linter);
    ASSERT_TRUE(linter != NULL);
    EXPECT_STREQ("LLVM version 22.1.8", linter->version);

    fixture_teardown(&fixture);
}

MOLTEST(tools_keeps_a_version_worded_without_the_word) {
    tools_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    /* cppcheck says "Cppcheck 2.21.0" and nothing else; the fallback to the
       first line is what makes that work. */
    ASSERT_TRUE(plant_working(&fixture, "cppcheck", "Cppcheck 2.21.0"));

    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);

    const dev_tool *linter = of_kind(found, count, tool_linter);
    ASSERT_TRUE(linter != NULL);
    EXPECT_STREQ("Cppcheck 2.21.0", linter->version);

    fixture_teardown(&fixture);
}

MOLTEST(tools_says_where_a_tool_came_from) {
    tools_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));
    /* PATH is the fixture, so anything found there is the machine's own. */
    ASSERT_TRUE(plant_working(&fixture, "clang-format", "clang-format version 22.1.8"));

    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);

    const dev_tool *formatter = of_kind(found, count, tool_formatter);
    ASSERT_TRUE(formatter != NULL);
    EXPECT_EQ(toolchain_source_system, formatter->source);
    /* And the path, which is the whole reason a caller asks. */
    EXPECT_TRUE(strstr(formatter->path, fixture.root) != NULL);
    EXPECT_TRUE(strstr(formatter->path, "clang-format") != NULL);

    fixture_teardown(&fixture);
}
