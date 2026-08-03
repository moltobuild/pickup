#include <moltest.h>

#include <pickup/detect/gcc_install.h>

#include <stdio.h>
#include <string.h>

/*
 * These tests are about reading a decision the driver never prints anywhere
 * else. The text is what clang 22.1.8 actually wrote on the machine this was
 * developed on, noise included: recorded rather than produced, so the parsing
 * stays checkable on a machine with a different set of compilers installed.
 */
static const char clang_verbose[] =
    "clang version 22.1.8\n"
    "Target: x86_64-unknown-linux-gnu\n"
    "Thread model: posix\n"
    "clang: warning: future releases of the clang compiler will prefer GCC "
    "installations containing libstdc++ include directories; "
    "'/usr/lib/gcc/x86_64-linux-gnu/11' would be chosen over "
    "'/usr/lib/gcc/x86_64-linux-gnu/12' [-Wgcc-install-dir-libstdcxx]\n"
    "Found candidate GCC installation: /usr/lib/gcc/x86_64-linux-gnu/11\n"
    "Found candidate GCC installation: /usr/lib/gcc/x86_64-linux-gnu/12\n"
    "Found candidate GCC installation: /usr/lib/gcc/x86_64-linux-gnu/9\n"
    "Selected GCC installation: /usr/lib/gcc/x86_64-linux-gnu/12\n"
    "Candidate multilib: .;@m64\n"
    "Selected multilib: .;@m64\n";

MOLTEST(gcc_install_reads_every_candidate) {
    gcc_install_list list;
    ASSERT_TRUE(gcc_install_parse(clang_verbose, &list));

    ASSERT_EQ(3, (int)list.count);
    EXPECT_STREQ("/usr/lib/gcc/x86_64-linux-gnu/11", list.items[0].path);
    EXPECT_STREQ("/usr/lib/gcc/x86_64-linux-gnu/12", list.items[1].path);
    EXPECT_STREQ("/usr/lib/gcc/x86_64-linux-gnu/9", list.items[2].path);
}

MOLTEST(gcc_install_reads_which_one_was_chosen) {
    gcc_install_list list;
    ASSERT_TRUE(gcc_install_parse(clang_verbose, &list));

    /* The point of the whole exercise: the driver picked the highest number,
       which is the one that is missing its C++ half. */
    ASSERT_TRUE(list.selected != GCC_INSTALL_NONE);
    EXPECT_STREQ("/usr/lib/gcc/x86_64-linux-gnu/12", list.items[list.selected].path);
}

MOLTEST(gcc_install_does_not_mistake_the_warning_for_a_candidate) {
    gcc_install_list list;
    ASSERT_TRUE(gcc_install_parse(clang_verbose, &list));

    /* The warning quotes two of the same paths. Counting them would report
       five installations where there are three. */
    EXPECT_EQ(3, (int)list.count);
}

MOLTEST(gcc_install_reports_the_same_installation_once) {
    /* Clang lists an installation once per search prefix it was found under. */
    static const char repeated[] =
        "Found candidate GCC installation: /usr/lib/gcc/x86_64-linux-gnu/11\n"
        "Found candidate GCC installation: /usr/lib/gcc/x86_64-linux-gnu/11\n"
        "Selected GCC installation: /usr/lib/gcc/x86_64-linux-gnu/11\n";

    gcc_install_list list;
    ASSERT_TRUE(gcc_install_parse(repeated, &list));
    EXPECT_EQ(1, (int)list.count);
    EXPECT_EQ(0, (int)list.selected);
}

MOLTEST(gcc_install_selection_is_recorded_even_when_never_listed) {
    /* A driver that names only its choice still has to be understood. */
    static const char only_selected[] =
        "Selected GCC installation: /usr/lib/gcc/x86_64-linux-gnu/13\n";

    gcc_install_list list;
    ASSERT_TRUE(gcc_install_parse(only_selected, &list));
    ASSERT_EQ(1, (int)list.count);
    EXPECT_EQ(0, (int)list.selected);
    EXPECT_STREQ("/usr/lib/gcc/x86_64-linux-gnu/13", list.items[0].path);
}

MOLTEST(gcc_install_finds_no_gcc_in_a_compiler_that_reports_none) {
    /* Every non-Clang driver, and Clang on a machine with no GCC at all. An
       empty list is an answer; it must not read as a failure. */
    gcc_install_list list;
    ASSERT_TRUE(gcc_install_parse("gcc version 12.3.0 (Ubuntu)\n", &list));
    EXPECT_EQ(0, (int)list.count);
    EXPECT_TRUE(list.selected == GCC_INSTALL_NONE);
}

MOLTEST(gcc_install_refuses_nothing_at_all) {
    gcc_install_list list;
    EXPECT_FALSE(gcc_install_parse(NULL, &list));
    EXPECT_EQ(0, (int)list.count);
}

MOLTEST(gcc_install_best_prefers_the_highest_that_has_libstdcxx) {
    /* Built by hand rather than parsed: what is under test is the choice, and
       the disk on the test machine must not decide the outcome. */
    gcc_install_list list = { .count = 3, .selected = 1 };
    snprintf(list.items[0].path, sizeof list.items[0].path, "%s",
             "/usr/lib/gcc/x86_64-linux-gnu/11");
    list.items[0].has_libstdcxx = true;
    snprintf(list.items[1].path, sizeof list.items[1].path, "%s",
             "/usr/lib/gcc/x86_64-linux-gnu/12");
    list.items[1].has_libstdcxx = false;  /* the one the driver chose */
    snprintf(list.items[2].path, sizeof list.items[2].path, "%s",
             "/usr/lib/gcc/x86_64-linux-gnu/9");
    list.items[2].has_libstdcxx = true;

    gcc_install best;
    ASSERT_TRUE(gcc_install_best(&list, &best));

    /* 12 is higher and 9 has libstdc++ too, so this is both rules at once:
       only complete installations count, and among those the highest wins. */
    EXPECT_STREQ("/usr/lib/gcc/x86_64-linux-gnu/11", best.path);
}

MOLTEST(gcc_install_best_finds_nothing_when_none_is_complete) {
    gcc_install_list list = { .count = 1, .selected = 0 };
    snprintf(list.items[0].path, sizeof list.items[0].path, "%s",
             "/usr/lib/gcc/x86_64-linux-gnu/12");
    list.items[0].has_libstdcxx = false;

    gcc_install best;
    /* This is the state that leaves a clang unable to compile C++ through
       libstdc++ at all, and it has to be distinguishable from success. */
    EXPECT_FALSE(gcc_install_best(&list, &best));
}
