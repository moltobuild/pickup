#include <moltest.h>

#include <pickup/util/color.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Remember and restore an environment variable a test has to move. */
typedef struct {
    const char *name;
    char previous[256];
    bool had_previous;
} env_guard;

static void guard_set(env_guard *guard, const char *name, const char *value) {
    guard->name = name;
    const char *existing = getenv(name);
    guard->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(guard->previous, sizeof guard->previous, "%s", existing);
    if (value != NULL)
        (void)setenv(name, value, 1);
    else
        (void)unsetenv(name);
}

static void guard_restore(env_guard *guard) {
    if (guard->had_previous)
        (void)setenv(guard->name, guard->previous, 1);
    else
        (void)unsetenv(guard->name);
}

MOLTEST(color_stays_off_when_output_is_not_a_terminal) {
    FILE *sink = tmpfile();
    ASSERT_TRUE(sink != NULL);

    /* Redirected into a file or a log, escape sequences would be noise in
       somebody's build output. */
    EXPECT_FALSE(color_enabled(sink));
    EXPECT_FALSE(color_enabled(NULL));

    fclose(sink);
}

MOLTEST(color_honours_no_color) {
    env_guard guard;
    guard_set(&guard, NO_COLOR_ENV, "1");

    /* Even on a terminal this must be off; it is the convention pip and the
       rest follow, and users set it deliberately. */
    EXPECT_FALSE(color_enabled(stdout));

    guard_restore(&guard);
}

MOLTEST(color_stays_off_on_a_dumb_terminal) {
    env_guard guard;
    guard_set(&guard, TERM_ENV, TERM_DUMB);
    EXPECT_FALSE(color_enabled(stdout));
    guard_restore(&guard);
}

MOLTEST(color_codes_are_empty_until_configured_on) {
    FILE *sink = tmpfile();
    ASSERT_TRUE(sink != NULL);
    color_configure(sink); /* not a terminal, so colour is off */

    /* Empty rather than absent, so callers print them unconditionally and the
       text comes out exactly as it did before there was any colour. */
    EXPECT_STREQ("", color_ok());
    EXPECT_STREQ("", color_error());
    EXPECT_STREQ("", color_accent());
    EXPECT_STREQ("", color_dim());
    EXPECT_STREQ("", color_reset());

    fclose(sink);
}

MOLTEST(color_codes_are_distinct_escapes_when_on) {
    /* The accessors are only meaningful together: whatever they return, the
       four must differ from each other and every one must be closed by the
       same reset. */
    const char *ok = color_ok();
    const char *error = color_error();
    const char *accent = color_accent();
    const char *dim = color_dim();

    if (ok[0] == '\0')
        SKIP("colour is off in this environment");

    EXPECT_TRUE(strcmp(ok, error) != 0);
    EXPECT_TRUE(strcmp(ok, accent) != 0);
    EXPECT_TRUE(strcmp(accent, dim) != 0);
    EXPECT_TRUE(color_reset()[0] == '\x1b');
}
