#include <moltest.h>

#include <pickup/detect/scanner.h>
#include <pickup/util/str_list.h>

#include <stdlib.h>
#include <string.h>

MOLTEST(scanner_recognizes_compiler_names) {
    /* Plain and versioned forms. */
    EXPECT_TRUE(scanner_is_candidate_name("gcc"));
    EXPECT_TRUE(scanner_is_candidate_name("gcc-12"));
    EXPECT_TRUE(scanner_is_candidate_name("clang"));
    EXPECT_TRUE(scanner_is_candidate_name("clang-14"));
    EXPECT_TRUE(scanner_is_candidate_name("g++"));

    /* The conventional entry points. */
    EXPECT_TRUE(scanner_is_candidate_name("cc"));
    EXPECT_TRUE(scanner_is_candidate_name("c++"));

    /* Cross compilers carry a triple in front. */
    EXPECT_TRUE(scanner_is_candidate_name("arm-none-eabi-gcc"));
    EXPECT_TRUE(scanner_is_candidate_name("x86_64-linux-gnu-g++-11"));
}

MOLTEST(scanner_rejects_names_that_only_look_like_compilers) {
    /* A prefix must end the name or be followed by a version separator, so
       unrelated tools are not probed. */
    EXPECT_FALSE(scanner_is_candidate_name("gccmakedep"));
    EXPECT_FALSE(scanner_is_candidate_name("clangd"));
    EXPECT_FALSE(scanner_is_candidate_name("ccache"));
    EXPECT_FALSE(scanner_is_candidate_name("make"));
    EXPECT_FALSE(scanner_is_candidate_name(""));
}

MOLTEST(scanner_reports_each_compiler_once) {
    str_list found;
    str_list_init(&found);
    /* /usr/bin holds cc, gcc and gcc-N pointing at the same binaries; each
       compiler must appear once however many names reach it. */
    ASSERT_TRUE(scanner_collect("/usr/bin", &found));

    for (size_t i = 0; i < str_list_count(&found); i++) {
        for (size_t j = i + 1; j < str_list_count(&found); j++)
            EXPECT_FALSE(strcmp(str_list_get(&found, i), str_list_get(&found, j)) == 0);
    }
    str_list_free(&found);
}

MOLTEST(scanner_survives_directories_that_do_not_exist) {
    str_list found;
    str_list_init(&found);
    /* A PATH entry that is missing or unreadable is normal, not fatal. */
    EXPECT_TRUE(scanner_collect("/nonexistent:/also/missing", &found));
    EXPECT_EQ(0, str_list_count(&found));
    EXPECT_TRUE(scanner_collect(NULL, &found));
    str_list_free(&found);
}
