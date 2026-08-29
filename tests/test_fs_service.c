#include <moltest.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>

#include <stdio.h>
#include <string.h>

/*
 * The two questions fs_service answers that have a different answer on each
 * platform. Both were POSIX calls spread through the tree before, and both are
 * here now so there is one place to change when a third platform disagrees.
 */

/* --- fs_real_path --- */

MOLTEST(a_real_path_is_absolute_and_resolved) {
    char root[PICKUP_PATHS_MAX];
    ASSERT_TRUE(moltest_temp_dir("pickup_realpath", root, sizeof root));

    char nested[PICKUP_PATHS_MAX];
    snprintf(nested, sizeof nested, "%s/one/two", root);
    ASSERT_TRUE(fs_make_dirs(nested));

    /* The point of resolving: `..` is gone from the answer, and the answer is
       a path something else could open. */
    char winding[PICKUP_PATHS_MAX];
    snprintf(winding, sizeof winding, "%s/one/two/../two", root);

    char resolved[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_real_path(winding, resolved, sizeof resolved));
    EXPECT_NULL(strstr(resolved, ".."));
    EXPECT_TRUE(fs_is_dir(resolved));

    EXPECT_TRUE(fs_remove_tree(root));
}

MOLTEST(a_path_that_is_not_there_does_not_resolve) {
    char resolved[PICKUP_PATHS_MAX];
    EXPECT_FALSE(fs_real_path("/pickup/no/such/place/at/all", resolved, sizeof resolved));
}

/* An answer that does not fit is refused rather than cut: half a path names a
   different file, and a caller that trusted it would open the wrong one. */
MOLTEST(a_real_path_that_does_not_fit_is_refused) {
    char root[PICKUP_PATHS_MAX];
    ASSERT_TRUE(moltest_temp_dir("pickup_realpath_small", root, sizeof root));

    char tiny[4];
    EXPECT_FALSE(fs_real_path(root, tiny, sizeof tiny));

    EXPECT_TRUE(fs_remove_tree(root));
}

/* --- fs_executable_name --- */

/*
 * What a file has to be called to be worth running.
 *
 * The two platforms disagree completely, so the test does too: on POSIX the
 * name is the name and the execute bit says the rest, while on Windows the
 * suffix is the permission — which is why `gcc.exe` is a candidate there and
 * `gcc` is not.
 */
MOLTEST(an_executable_name_drops_what_the_platform_appends) {
    char bare[64];

#ifdef _WIN32
    ASSERT_TRUE(fs_executable_name("gcc.exe", bare, sizeof bare));
    EXPECT_STREQ("gcc", bare);

    /* A filesystem that does not distinguish case will hand back either. */
    ASSERT_TRUE(fs_executable_name("CLANG.EXE", bare, sizeof bare));
    EXPECT_STREQ("CLANG", bare);

    /* Nothing to run: on Windows the suffix is what says a file may be, so its
       absence is a refusal and not a name to match against. */
    EXPECT_FALSE(fs_executable_name("gcc", bare, sizeof bare));
    EXPECT_FALSE(fs_executable_name("notes.txt", bare, sizeof bare));
    EXPECT_FALSE(fs_executable_name(".exe", bare, sizeof bare));
#else
    ASSERT_TRUE(fs_executable_name("gcc", bare, sizeof bare));
    EXPECT_STREQ("gcc", bare);

    /* Nothing is stripped here: a POSIX file called `gcc.exe` is called
       `gcc.exe`, and pretending otherwise would invent a compiler. */
    ASSERT_TRUE(fs_executable_name("gcc.exe", bare, sizeof bare));
    EXPECT_STREQ("gcc.exe", bare);
#endif
}

MOLTEST(an_executable_name_that_does_not_fit_is_refused) {
    char bare[4];
    EXPECT_FALSE(fs_executable_name("a-very-long-compiler-name", bare, sizeof bare));
}
