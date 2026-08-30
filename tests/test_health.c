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

/* Plant a program that behaves as `behaviour` says and return it as a driver. */
static bool fake_setup(fake_compiler *fake, const char *behaviour) {
    if (!moltest_temp_dir("pickup_health_t", fake->root, sizeof fake->root))
        return false;

    char at[PICKUP_PATHS_MAX];
    if (!fs_format_path(at, sizeof at, "%s/fakecc", fake->root))
        return false;

    char spec[128];
    snprintf(spec, sizeof spec, "behave %s\n", behaviour);
    return moltest_fake_program(at, spec, fake->driver, sizeof fake->driver);
}

static void fake_teardown(fake_compiler *fake) {
    (void)fs_remove_tree(fake->root);
}

/*
 * The compilers these tests stand up.
 *
 * Written in C rather than as shell scripts, because a fake has to be a program
 * the platform can actually start and `#!/bin/sh` is not one on Windows. Each
 * says only where it gives up; the arguments they read are the ones a real
 * driver reads.
 */

/* Whether the driver was asked only to parse, and where it was told to write. */
static bool asked_only_to_parse(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-fsyntax-only") == 0)
            return true;
    }
    return false;
}

static const char *told_to_write_to(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-o") == 0)
            return argv[i + 1];
    }
    return NULL;
}

static bool was_given(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return true;
    }
    return false;
}

/* Refuses everything, the way a compiler does when <iostream> is nowhere to be
   found. */
MOLTEST_FAKE(cc_refuses_everything) {
    (void)argc;
    (void)argv;
    return 1;
}

/* Parses and will not link: the headers are there and the runtime is not. */
MOLTEST_FAKE(cc_parses_but_will_not_link) {
    return asked_only_to_parse(argc, argv) ? 0 : 1;
}

/* Links, and what it produced cannot be executed. The case that motivated the
   module: nothing stopping at the compiler's exit status would notice. */
MOLTEST_FAKE(cc_links_something_that_will_not_start) {
    if (asked_only_to_parse(argc, argv))
        return 0;
    const char *out = told_to_write_to(argc, argv);
    if (out != NULL)
        (void)fs_write_file(out, "not an executable\n");
    return 0;
}

/* Links something that starts and exits cleanly. */
MOLTEST_FAKE(cc_links_and_runs) {
    if (asked_only_to_parse(argc, argv))
        return 0;
    const char *out = told_to_write_to(argc, argv);
    if (out != NULL && !moltest_fake_program(out, "exit 0\n", NULL, 0))
        return 1;
    return 0;
}

/* Links, and leaves a note of where it wrote, beside itself. The note is how a
   test can ask whether the probe cleaned its temporary up: `/tmp` used to be
   the meeting point, and `/tmp` is not a place on every platform. */
MOLTEST_FAKE(cc_links_and_says_where) {
    if (asked_only_to_parse(argc, argv))
        return 0;
    const char *out = told_to_write_to(argc, argv);
    if (out == NULL)
        return 1;
    if (!moltest_fake_program(out, "exit 0\n", NULL, 0))
        return 1;

    char witness[PICKUP_PATHS_MAX];
    if (!fs_format_path(witness, sizeof witness, "%s.witness", argv[0]))
        return 1;
    return fs_write_file(witness, out) ? 0 : 1;
}

/* Works only when told which standard library to use: what proves that a
   candidate's flags reach the compiler. */
MOLTEST_FAKE(cc_needs_the_libcxx_flag) {
    if (!was_given(argc, argv, "-stdlib=libc++"))
        return 1;
    if (asked_only_to_parse(argc, argv))
        return 0;
    const char *out = told_to_write_to(argc, argv);
    if (out != NULL && !moltest_fake_program(out, "exit 0\n", NULL, 0))
        return 1;
    return 0;
}

MOLTEST(health_reports_no_driver_when_there_is_none) {
    /* An empty cxx_path is the ordinary case of a C compiler with no C++ side,
       and must not read as a compiler that failed. */
    EXPECT_EQ(health_no_driver, health_probe(NULL, lang_c, NULL, 0, true));
    EXPECT_EQ(health_no_driver, health_probe("", lang_c, NULL, 0, true));
}

MOLTEST(health_reports_no_driver_when_the_binary_cannot_run) {
    /* A path that names nothing executable is an absent compiler, not one
       whose standard library is incomplete. Reporting the latter would send a
       reader looking for a package that was never the problem. */
    EXPECT_EQ(health_no_driver,
              health_probe("/nonexistent/pickup/cc", lang_c, NULL, 0, true));
}

MOLTEST(health_reports_missing_headers_when_the_include_fails) {
    fake_compiler fake;
    /* Refuses everything, the way a compiler does when <iostream> is nowhere
       to be found. */
    ASSERT_TRUE(fake_setup(&fake, "cc_refuses_everything"));

    EXPECT_EQ(health_no_headers, health_probe(fake.driver, lang_cxx, NULL, 0, true));

    fake_teardown(&fake);
}

MOLTEST(health_reports_no_link_when_only_the_parse_succeeds) {
    fake_compiler fake;
    /* Parses and will not link: the standard library's headers are present and
       the runtime it needs is not. */
    ASSERT_TRUE(fake_setup(&fake, "cc_parses_but_will_not_link"));

    EXPECT_EQ(health_no_link, health_probe(fake.driver, lang_cxx, NULL, 0, true));

    fake_teardown(&fake);
}

MOLTEST(health_reports_no_run_when_the_executable_will_not_start) {
    fake_compiler fake;
    /* The case that motivated the whole module: the link exits zero, and what
       it produced cannot be executed. Nothing that stops at the exit status of
       the compiler would ever notice. */
    ASSERT_TRUE(fake_setup(&fake, "cc_links_something_that_will_not_start"));

    EXPECT_EQ(health_no_run, health_probe(fake.driver, lang_cxx, NULL, 0, true));

    fake_teardown(&fake);
}

MOLTEST(health_reports_ok_only_when_the_program_actually_ran) {
    fake_compiler fake;
    ASSERT_TRUE(fake_setup(&fake, "cc_links_and_runs"));

    EXPECT_EQ(health_ok, health_probe(fake.driver, lang_cxx, NULL, 0, true));
    EXPECT_TRUE(health_is_usable(health_ok));

    fake_teardown(&fake);
}

MOLTEST(health_passes_the_flags_it_was_given_to_the_compiler) {
    /* Succeeds only when -stdlib=libc++ is on the command line. This is the
       claim recipe_discover rests on: a candidate set of flags is proven by
       handing them to the compiler, so they have to arrive. */
    fake_compiler flagged;
    ASSERT_TRUE(fake_setup(&flagged, "cc_needs_the_libcxx_flag"));

    const char *flags[] = { "-stdlib=libc++" };
    EXPECT_EQ(health_ok, health_probe(flagged.driver, lang_cxx, flags, 1, true));
    /* And without them the very same compiler is unusable. */
    EXPECT_EQ(health_no_headers, health_probe(flagged.driver, lang_cxx, NULL, 0, true));

    fake_teardown(&flagged);
}

MOLTEST(health_leaves_nothing_behind) {
    fake_compiler fake;
    ASSERT_TRUE(fake_setup(&fake, "cc_links_and_says_where"));

    ASSERT_EQ(health_ok, health_probe(fake.driver, lang_cxx, NULL, 0, true));

    /* The linked program is a temporary, and a probe that littered /tmp on
       every run of every command would be its own kind of bug. */
    char note[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(note, sizeof note, "%s.witness", fake.driver));
    char *witness = fs_read_file(note);
    ASSERT_TRUE(witness != NULL);
    EXPECT_FALSE(fs_path_exists(witness));
    free(witness);
    (void)remove(note);

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

/*
 * What changes when the program is not for this machine.
 *
 * A cross compiler links something this machine cannot start, and a probe that
 * insisted on running it would call every cross compiler broken. So the caller
 * says which bar applies, and the same fake — one that links something
 * unrunnable — is a failure under one and a success under the other.
 */
MOLTEST(health_asks_only_that_it_linked_when_the_output_is_not_for_here) {
    fake_compiler fake;
    ASSERT_TRUE(fake_setup(&fake, "cc_links_something_that_will_not_start"));

    /* Expected to run here: what it produced does not start, and that is the
       failure the module exists to catch. */
    EXPECT_EQ(health_no_run, health_probe(fake.driver, lang_cxx, NULL, 0, true));

    /* Not expected to run here: it linked, which is all that can be asked of a
       compiler emitting for somewhere else. */
    EXPECT_EQ(health_ok, health_probe(fake.driver, lang_cxx, NULL, 0, false));

    fake_teardown(&fake);
}

/* And the relaxation is not a blanket pass: a compiler that cannot link is
   still broken, whoever the output was for. */
MOLTEST(health_still_refuses_one_that_cannot_link_for_elsewhere) {
    fake_compiler fake;
    ASSERT_TRUE(fake_setup(&fake, "cc_parses_but_will_not_link"));

    EXPECT_EQ(health_no_link, health_probe(fake.driver, lang_cxx, NULL, 0, false));

    fake_teardown(&fake);
}
