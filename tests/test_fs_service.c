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

/* --- fs_walk_path --- */

typedef struct {
    char seen[8][PICKUP_PATHS_MAX];
    size_t count;
    size_t stop_after; /* 0 means never stop */
} walk_record;

static bool record_directory(const char *directory, void *context) {
    walk_record *record = context;
    if (record->count < 8)
        snprintf(record->seen[record->count], PICKUP_PATHS_MAX, "%s", directory);
    record->count++;
    return record->stop_after == 0 || record->count < record->stop_after;
}

/*
 * The separator is the platform's, and this is the case that says why.
 *
 * A colon belongs to a drive letter on Windows, so splitting `C:\a;C:\b` on one
 * yields four fragments that are not directories — which is exactly how pickup
 * came to report "no compilers found on PATH" on a machine with a compiler.
 */
MOLTEST(walking_path_splits_on_what_the_platform_separates_with) {
    walk_record record = {0};
#ifdef _WIN32
    EXPECT_TRUE(fs_walk_path("C:\\one;C:\\two", record_directory, &record));
    ASSERT_EQ((size_t)2, record.count);
    EXPECT_STREQ("C:\\one", record.seen[0]);
    EXPECT_STREQ("C:\\two", record.seen[1]);
#else
    EXPECT_TRUE(fs_walk_path("/one:/two", record_directory, &record));
    ASSERT_EQ((size_t)2, record.count);
    EXPECT_STREQ("/one", record.seen[0]);
    EXPECT_STREQ("/two", record.seen[1]);
#endif
}

/* An empty entry is not the current directory: PATH says nothing there, and
   visiting `.` would search wherever the caller happened to be standing. */
MOLTEST(walking_path_skips_the_gaps) {
    walk_record record = {0};
#ifdef _WIN32
    EXPECT_TRUE(fs_walk_path(";C:\\one;;C:\\two;", record_directory, &record));
#else
    EXPECT_TRUE(fs_walk_path(":/one::/two:", record_directory, &record));
#endif
    EXPECT_EQ((size_t)2, record.count);
}

MOLTEST(walking_path_stops_when_the_visitor_says_so) {
    walk_record record = {.stop_after = 2};
#ifdef _WIN32
    EXPECT_FALSE(fs_walk_path("C:\\a;C:\\b;C:\\c", record_directory, &record));
#else
    EXPECT_FALSE(fs_walk_path("/a:/b:/c", record_directory, &record));
#endif
    EXPECT_EQ((size_t)2, record.count);
}

/* No PATH is not an error: it is a machine with nothing on it, which the
   caller reports as finding nothing rather than as failing. */
MOLTEST(walking_no_path_at_all_visits_nothing) {
    walk_record record = {0};
    EXPECT_TRUE(fs_walk_path(NULL, record_directory, &record));
    EXPECT_EQ((size_t)0, record.count);
}

/* --- fs_program_name and fs_executable_file --- */

/*
 * The name a compiler is known by, against the file it is stored in.
 *
 * Everything that ranks or transforms a driver reads the name: `gcc` is
 * preferred over `c++`, and the C++ driver is found by turning `gcc` into
 * `g++`. A name still carrying `.exe` matches neither list, which is how a
 * toolchain came to be recorded with a `c++` chosen to compile C.
 */
MOLTEST(a_program_name_is_the_last_component_without_the_platforms_suffix) {
    char name[64];

    ASSERT_TRUE(fs_program_name("/usr/bin/gcc", name, sizeof name));
    EXPECT_STREQ("gcc", name);

    ASSERT_TRUE(fs_program_name("gcc", name, sizeof name));
    EXPECT_STREQ("gcc", name);

#ifdef _WIN32
    ASSERT_TRUE(fs_program_name("C:\\msys64\\mingw64\\bin\\gcc.exe", name, sizeof name));
    EXPECT_STREQ("gcc", name);

    /* The scanner composes with a forward slash, so a real path arrives with
       both kinds in it. */
    ASSERT_TRUE(fs_program_name("C:\\mingw64\\bin/c++.exe", name, sizeof name));
    EXPECT_STREQ("c++", name);
#else
    /* Nothing is stripped, and a backslash is an ordinary character here: a
       POSIX file called `weird\\gcc.exe` is called exactly that. */
    ASSERT_TRUE(fs_program_name("/usr/bin/gcc.exe", name, sizeof name));
    EXPECT_STREQ("gcc.exe", name);
#endif
}

MOLTEST(a_program_name_that_does_not_fit_is_refused) {
    char name[4];
    EXPECT_FALSE(fs_program_name("/usr/bin/a-long-compiler-name", name, sizeof name));
}

MOLTEST(an_executable_file_carries_what_the_platform_appends) {
    char file[64];
    ASSERT_TRUE(fs_executable_file("g++", file, sizeof file));
#ifdef _WIN32
    EXPECT_STREQ("g++.exe", file);
#else
    EXPECT_STREQ("g++", file);
#endif
}

/* The two are inverses, which is the property the C++ driver lookup relies on:
   a name is turned into a file, the file is found, and the file is read back
   as the name it stands for. */
MOLTEST(a_name_survives_the_trip_through_a_filename) {
    char file[64];
    char back[64];
    ASSERT_TRUE(fs_executable_file("clang++", file, sizeof file));
    ASSERT_TRUE(fs_program_name(file, back, sizeof back));
    EXPECT_STREQ("clang++", back);
}

/* --- fs_temp_file and fs_temp_program --- */

/*
 * Where a probe puts something it is about to throw away.
 *
 * `/tmp` is not a place on Windows, and a health probe that could not find one
 * reported "this compiler cannot link" — which is how a working gcc came to be
 * rejected with nothing listed as missing.
 */
MOLTEST(a_temporary_file_is_made_where_the_platform_keeps_them) {
    char first[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_temp_file("pickup_probe", first, sizeof first));
    EXPECT_TRUE(fs_path_exists(first));

    /* Two calls do not collide: the second is asked for while the first still
       exists, which is the case that matters to a caller about to write to
       both. */
    char second[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_temp_file("pickup_probe", second, sizeof second));
    EXPECT_STRNE(first, second);

    EXPECT_EQ(0, remove(first));
    EXPECT_EQ(0, remove(second));
}

/* A probe links a program here and then runs it, and on Windows what makes a
   file runnable is its name. */
MOLTEST(a_temporary_program_is_named_so_it_can_be_run) {
    char path[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_temp_program("pickup_probe", path, sizeof path));
    EXPECT_TRUE(fs_path_exists(path));

    const size_t length = strlen(path);
#ifdef _WIN32
    ASSERT_TRUE(length > 4);
    EXPECT_STREQ(".exe", path + length - 4);
#else
    /* Nothing is appended: the execute bit says it, not the name. */
    EXPECT_NULL(strstr(path, ".exe"));
#endif
    EXPECT_EQ(0, remove(path));
}

MOLTEST(a_temporary_path_that_does_not_fit_is_refused) {
    char tiny[8];
    EXPECT_FALSE(fs_temp_file("a_rather_long_prefix", tiny, sizeof tiny));
    EXPECT_FALSE(fs_temp_program("a_rather_long_prefix", tiny, sizeof tiny));
}
