#include <moltest.h>

#include <pickup/detect/health.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * The five outcomes are the point of this module, and telling them apart is
 * what makes a diagnosis useful rather than just negative. Real compilers can
 * only produce whichever ones the machine happens to be in a position to
 * produce, so the drivers here are stand-ins: shell scripts that fail at one
 * chosen step and succeed at every earlier one.
 *
 * That is what makes the test deterministic. It also lets the hardest case be
 * exercised at all — a link that exits zero and an executable that will not
 * start is not something one can arrange by picking a compiler.
 */

typedef struct {
    char root[64];
    char driver[PICKUP_PATHS_MAX];
} fake_compiler;

/* Write `body` as an executable script and return it as a usable driver. */
static bool fake_setup(fake_compiler *fake, const char *body) {
    if (!moltest_temp_dir("pickup_health_t", fake->root, sizeof fake->root))
        return false;
    if (!fs_format_path(fake->driver, sizeof fake->driver, "%s/fakecc", fake->root))
        return false;
    if (!fs_write_file(fake->driver, body))
        return false;
    return chmod(fake->driver, 0700) == 0;
}

static void fake_teardown(fake_compiler *fake) {
    (void)fs_remove_tree(fake->root);
}

/* Reads the program on stdin and reports which step it is being asked for, so
   each script below only has to say where it gives up. */
#define FAKE_PROLOGUE                                                          \
    "#!/bin/sh\n"                                                              \
    "out=''\n"                                                                 \
    "syntax=no\n"                                                              \
    "while [ $# -gt 0 ]; do\n"                                                 \
    "  case \"$1\" in\n"                                                       \
    "    -fsyntax-only) syntax=yes ;;\n"                                       \
    "    -o) shift; out=\"$1\" ;;\n"                                           \
    "  esac\n"                                                                 \
    "  shift\n"                                                                \
    "done\n"                                                                   \
    "cat > /dev/null\n"

MOLTEST(health_reports_no_driver_when_there_is_none) {
    /* An empty cxx_path is the ordinary case of a C compiler with no C++ side,
       and must not read as a compiler that failed. */
    EXPECT_EQ(health_no_driver, health_probe(NULL, lang_c, NULL, 0));
    EXPECT_EQ(health_no_driver, health_probe("", lang_c, NULL, 0));
}

MOLTEST(health_reports_no_driver_when_the_binary_cannot_run) {
    /* A path that names nothing executable is an absent compiler, not one
       whose standard library is incomplete. Reporting the latter would send a
       reader looking for a package that was never the problem. */
    EXPECT_EQ(health_no_driver,
              health_probe("/nonexistent/pickup/cc", lang_c, NULL, 0));
}

MOLTEST(health_reports_missing_headers_when_the_include_fails) {
    fake_compiler fake;
    /* Refuses everything, the way a compiler does when <iostream> is nowhere
       to be found. */
    ASSERT_TRUE(fake_setup(&fake, FAKE_PROLOGUE "exit 1\n"));

    EXPECT_EQ(health_no_headers, health_probe(fake.driver, lang_cxx, NULL, 0));

    fake_teardown(&fake);
}

MOLTEST(health_reports_no_link_when_only_the_parse_succeeds) {
    fake_compiler fake;
    /* Parses and will not link: the standard library's headers are present and
       the runtime it needs is not. */
    ASSERT_TRUE(fake_setup(&fake,
        FAKE_PROLOGUE
        "[ \"$syntax\" = yes ] && exit 0\n"
        "exit 1\n"));

    EXPECT_EQ(health_no_link, health_probe(fake.driver, lang_cxx, NULL, 0));

    fake_teardown(&fake);
}

MOLTEST(health_reports_no_run_when_the_executable_will_not_start) {
    fake_compiler fake;
    /* The case that motivated the whole module: the link exits zero, and what
       it produced cannot be executed. Nothing that stops at the exit status of
       the compiler would ever notice. */
    ASSERT_TRUE(fake_setup(&fake,
        FAKE_PROLOGUE
        "[ \"$syntax\" = yes ] && exit 0\n"
        "echo 'not an executable' > \"$out\"\n"
        "chmod 600 \"$out\"\n"
        "exit 0\n"));

    EXPECT_EQ(health_no_run, health_probe(fake.driver, lang_cxx, NULL, 0));

    fake_teardown(&fake);
}

MOLTEST(health_reports_ok_only_when_the_program_actually_ran) {
    fake_compiler fake;
    ASSERT_TRUE(fake_setup(&fake,
        FAKE_PROLOGUE
        "[ \"$syntax\" = yes ] && exit 0\n"
        "printf '#!/bin/sh\\nexit 0\\n' > \"$out\"\n"
        "chmod 700 \"$out\"\n"
        "exit 0\n"));

    EXPECT_EQ(health_ok, health_probe(fake.driver, lang_cxx, NULL, 0));
    EXPECT_TRUE(health_is_usable(health_ok));

    fake_teardown(&fake);
}

MOLTEST(health_passes_the_flags_it_was_given_to_the_compiler) {
    /* Succeeds only when -stdlib=libc++ is on the command line. This is the
       claim recipe_discover rests on: a candidate set of flags is proven by
       handing them to the compiler, so they have to arrive. */
    fake_compiler flagged;
    ASSERT_TRUE(fake_setup(&flagged,
        "#!/bin/sh\n"
        "found=no\n"
        "out=''\n"
        "syntax=no\n"
        "while [ $# -gt 0 ]; do\n"
        "  case \"$1\" in\n"
        "    -fsyntax-only) syntax=yes ;;\n"
        "    -stdlib=libc++) found=yes ;;\n"
        "    -o) shift; out=\"$1\" ;;\n"
        "  esac\n"
        "  shift\n"
        "done\n"
        "cat > /dev/null\n"
        "[ \"$found\" = yes ] || exit 1\n"
        "[ \"$syntax\" = yes ] && exit 0\n"
        "printf '#!/bin/sh\\nexit 0\\n' > \"$out\"\n"
        "chmod 700 \"$out\"\n"
        "exit 0\n"));

    const char *flags[] = { "-stdlib=libc++" };
    EXPECT_EQ(health_ok, health_probe(flagged.driver, lang_cxx, flags, 1));
    /* And without them the very same compiler is unusable. */
    EXPECT_EQ(health_no_headers, health_probe(flagged.driver, lang_cxx, NULL, 0));

    fake_teardown(&flagged);
}

MOLTEST(health_leaves_nothing_behind) {
    fake_compiler fake;
    ASSERT_TRUE(fake_setup(&fake,
        FAKE_PROLOGUE
        "[ \"$syntax\" = yes ] && exit 0\n"
        "printf '#!/bin/sh\\nexit 0\\n' > \"$out\"\n"
        "chmod 700 \"$out\"\n"
        "printf '%s' \"$out\" > /tmp/pickup_health_witness\n"
        "exit 0\n"));

    ASSERT_EQ(health_ok, health_probe(fake.driver, lang_cxx, NULL, 0));

    /* The linked program is a temporary, and a probe that littered /tmp on
       every run of every command would be its own kind of bug. */
    char *witness = fs_read_file("/tmp/pickup_health_witness");
    ASSERT_TRUE(witness != NULL);
    EXPECT_FALSE(fs_path_exists(witness));
    free(witness);
    (void)remove("/tmp/pickup_health_witness");

    fake_teardown(&fake);
}

MOLTEST(health_describes_every_outcome) {
    /* doctor prints these, so none may be empty and none may read like
       another. */
    const health_status all[] = {
        health_ok, health_no_driver, health_no_headers, health_no_link, health_no_run,
    };
    size_t count = sizeof all / sizeof all[0];

    for (size_t i = 0; i < count; i++) {
        const char *message = health_status_message(all[i]);
        ASSERT_TRUE(message != NULL);
        EXPECT_TRUE(message[0] != '\0');
        for (size_t j = i + 1; j < count; j++)
            EXPECT_STRNE(message, health_status_message(all[j]));
    }

    EXPECT_TRUE(health_is_usable(health_ok));
    EXPECT_FALSE(health_is_usable(health_no_run));
    EXPECT_FALSE(health_is_usable(health_no_link));
}
