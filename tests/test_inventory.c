#include <moltest.h>

#include <stdio.h>

#include <pickup/detect/probe.h>
#include <pickup/services/inventory_service.h>

#include <string.h>
#include <unistd.h>

MOLTEST(probe_identifies_a_compiler_by_asking_it) {
    if (access("/usr/bin/gcc-12", X_OK) != 0)
        SKIP("gcc-12 is not installed");

    toolchain chain;
    ASSERT_TRUE(probe_identify("/usr/bin/gcc-12", &chain));

    /* Identity comes from the compiler's own macros, not from its filename. */
    EXPECT_EQ(vendor_gcc, chain.vendor);
    EXPECT_EQ(12, chain.version.major);
    EXPECT_STREQ("gcc-12", chain.name);
    EXPECT_STREQ("/usr/bin/gcc-12", chain.path);
    EXPECT_TRUE(strlen(chain.target) > 0);
}

MOLTEST(probe_rejects_something_that_is_not_a_compiler) {
    toolchain chain;
    EXPECT_FALSE(probe_identify("/bin/sh", &chain));
    EXPECT_FALSE(probe_identify("/nonexistent/compiler", &chain));
}

MOLTEST(probe_leaves_the_cxx_driver_empty_when_there_is_none) {
    if (access("/usr/bin/gcc-12", X_OK) != 0)
        SKIP("gcc-12 is not installed");
    if (access("/usr/bin/g++-12", X_OK) == 0)
        SKIP("this machine does have g++-12");

    toolchain chain;
    ASSERT_TRUE(probe_identify("/usr/bin/gcc-12", &chain));
    probe_find_cxx_driver(&chain);
    /* Guessing "g++-12" from "gcc-12" would name a binary that is not there;
       the field stays empty instead. */
    EXPECT_STREQ("", chain.cxx_path);
}

MOLTEST(inventory_discovers_and_orders_toolchains) {
    inventory list;
    ASSERT_TRUE(inventory_discover(&list));
    EXPECT_TRUE(list.count > 0); /* the machine that builds pickup has a compiler */

    /* Ordered by vendor, then newest first: two runs must agree. */
    for (size_t i = 1; i < list.count; i++) {
        const toolchain *previous = &list.items[i - 1];
        const toolchain *current = &list.items[i];
        int by_vendor = strcmp(toolchain_vendor_name(previous->vendor),
                               toolchain_vendor_name(current->vendor));
        EXPECT_TRUE(by_vendor <= 0);
        if (by_vendor == 0)
            EXPECT_TRUE(toolchain_version_compare(previous->version, current->version) >= 0);
    }
    inventory_free(&list);
}

MOLTEST(inventory_finds_a_toolchain_by_name_or_path) {
    inventory list;
    ASSERT_TRUE(inventory_discover(&list));
    ASSERT_TRUE(list.count > 0);

    const char *name = list.items[0].name;
    const toolchain *found = inventory_find(&list, name);
    ASSERT_NOT_NULL(found);
    EXPECT_STREQ(name, found->name);

    EXPECT_NOT_NULL(inventory_find(&list, list.items[0].path));
    EXPECT_NULL(inventory_find(&list, "definitely-not-a-compiler"));
    inventory_free(&list);
}

/*
 * Collapsing the aliases of one compiler.
 *
 * A single GCC answers to `cc`, `gcc-9`, `c89-gcc` and `g++-9`, and a list
 * that shows four rows for it says four things where there is one. Built by
 * hand rather than discovered: what is under test is the collapsing, and the
 * machine running the tests cannot be relied on to have the right mess
 * installed.
 */
static toolchain alias_of(const char *name, const char *path, const char *cxx,
                          int major, int minor, const char *target,
                          unsigned long long c_bits, unsigned long long cxx_bits) {
    toolchain chain = { 0 };
    snprintf(chain.name, sizeof chain.name, "%s", name);
    snprintf(chain.path, sizeof chain.path, "%s", path);
    snprintf(chain.cxx_path, sizeof chain.cxx_path, "%s", cxx);
    snprintf(chain.target, sizeof chain.target, "%s", target);
    chain.vendor = vendor_gcc;
    chain.version = (toolchain_version){ major, minor, 0 };
    chain.c_features.bits = c_bits;
    chain.cxx_features.bits = cxx_bits;
    return chain;
}

MOLTEST(inventory_collapses_the_aliases_of_one_compiler) {
    inventory list = { 0 };
    /* Four spellings of the same GCC, exactly as a real machine offers them. */
    toolchain cc = alias_of("cc", "/usr/bin/cc", "/usr/bin/c++", 9, 5,
                            "x86_64-linux-gnu", 0x1, 0x2);
    toolchain gpp = alias_of("g++-9", "/usr/bin/g++-9", "", 9, 5,
                             "x86_64-linux-gnu", 0x4, 0);
    toolchain c89 = alias_of("c89-gcc", "/usr/bin/c89-gcc", "", 9, 5,
                             "x86_64-linux-gnu", 0x8, 0);
    ASSERT_TRUE(inventory_append(&list, &gpp));
    ASSERT_TRUE(inventory_append(&list, &c89));
    ASSERT_TRUE(inventory_append(&list, &cc));

    inventory_settle(&list);

    ASSERT_EQ(1, (int)list.count);
    EXPECT_STREQ("gcc@9.5.0", list.items[0].id);
    /* The plain C driver is the one kept: the C++ driver is found by
       transforming its name, so keeping `g++-9` would lose the C++ side. */
    EXPECT_STREQ("cc", list.items[0].name);
    EXPECT_STREQ("/usr/bin/c++", list.items[0].cxx_path);
    /* And everything proven about any spelling survives: each was probed on
       its own, so taking one alone would report less than was measured. */
    EXPECT_TRUE(list.items[0].c_features.bits == (0x1 | 0x4 | 0x8));
    EXPECT_TRUE(list.items[0].cxx_features.bits == 0x2);

    inventory_free(&list);
}

MOLTEST(inventory_keeps_apart_two_compilers_of_the_same_version) {
    inventory list = { 0 };
    /* The system's 12.3.0 and a conda one. Same vendor and version, different
       toolchains, and collapsing them would hide one of them entirely. */
    toolchain system = alias_of("gcc-12", "/usr/bin/gcc-12", "", 12, 3,
                                "x86_64-linux-gnu", 0x1, 0);
    toolchain conda = alias_of("cc", "/opt/conda/bin/cc", "", 12, 3,
                               "x86_64-conda-linux-gnu", 0x1, 0);
    ASSERT_TRUE(inventory_append(&list, &system));
    ASSERT_TRUE(inventory_append(&list, &conda));

    inventory_settle(&list);

    ASSERT_EQ(2, (int)list.count);
    /* And their names say which is which without reading the target column. */
    bool saw_plain = false, saw_conda = false;
    for (size_t i = 0; i < list.count; i++) {
        saw_plain = saw_plain || strcmp(list.items[i].id, "gcc@12.3.0") == 0;
        saw_conda = saw_conda || strcmp(list.items[i].id, "gcc@12.3.0-conda") == 0;
    }
    EXPECT_TRUE(saw_plain);
    EXPECT_TRUE(saw_conda);

    inventory_free(&list);
}

MOLTEST(inventory_finds_a_toolchain_by_the_forms_a_person_types) {
    inventory list = { 0 };
    toolchain newer = alias_of("gcc-12", "/usr/bin/gcc-12", "", 12, 4,
                               "x86_64-linux-gnu", 0x1, 0);
    toolchain older = alias_of("gcc-11", "/usr/bin/gcc-11", "", 11, 4,
                               "x86_64-linux-gnu", 0x1, 0);
    ASSERT_TRUE(inventory_append(&list, &older));
    ASSERT_TRUE(inventory_append(&list, &newer));
    inventory_settle(&list);

    const toolchain *found = inventory_find(&list, "gcc@latest");
    ASSERT_TRUE(found != NULL);
    /* Ordered newest first, so the first match is the highest version. */
    EXPECT_STREQ("gcc@12.4.0", found->id);

    found = inventory_find(&list, "gcc@11");
    ASSERT_TRUE(found != NULL);
    EXPECT_STREQ("gcc@11.4.0", found->id);

    EXPECT_NULL(inventory_find(&list, "gcc@9"));
    EXPECT_EQ(2, (int)inventory_count_matching(&list, "gcc"));
    EXPECT_EQ(1, (int)inventory_count_matching(&list, "gcc@12"));

    inventory_free(&list);
}

/*
 * A cross toolchain names its drivers after the target triple and nothing
 * else: llvm-mingw ships `x86_64-w64-mingw32-gcc`, conda ships
 * `x86_64-conda-linux-gnu-gcc`, and install_service's own comment says as
 * much. The C++ driver beside it is the same name with the last component
 * transformed, so a match anchored at the start of the name finds nothing and
 * records the toolchain as having no C++ at all.
 */
MOLTEST(probe_pairs_a_prefixed_driver_with_its_cxx_sibling) {
    char root[PICKUP_PATH_MAX];
    ASSERT_TRUE(moltest_temp_dir("pickup_prefixed", root, sizeof root));

    char wanted[PICKUP_PATH_MAX];
    char c_driver[PICKUP_PATH_MAX];
    char cxx_driver[PICKUP_PATH_MAX];
    snprintf(wanted, sizeof wanted, "%s/x86_64-w64-mingw32-gcc", root);
    ASSERT_TRUE(moltest_fake_program(wanted, "exit 0\n", c_driver, sizeof c_driver));
    snprintf(wanted, sizeof wanted, "%s/x86_64-w64-mingw32-g++", root);
    ASSERT_TRUE(moltest_fake_program(wanted, "exit 0\n", cxx_driver, sizeof cxx_driver));

    /* The name is the driver's name, without whatever suffix the filesystem
       needed: a name is not a filename. */
    toolchain chain = {0};
    snprintf(chain.name, sizeof chain.name, "x86_64-w64-mingw32-gcc");
    snprintf(chain.path, sizeof chain.path, "%s", c_driver);

    probe_find_cxx_driver(&chain);
    EXPECT_STREQ(cxx_driver, chain.cxx_path);
}

/* The same for the clang spelling, which transforms differently: `clang` grows
   a `++` where `gcc` becomes `g++`. */
MOLTEST(probe_pairs_a_prefixed_clang_with_its_cxx_sibling) {
    char root[PICKUP_PATH_MAX];
    ASSERT_TRUE(moltest_temp_dir("pickup_prefixed_clang", root, sizeof root));

    char wanted[PICKUP_PATH_MAX];
    char c_driver[PICKUP_PATH_MAX];
    char cxx_driver[PICKUP_PATH_MAX];
    snprintf(wanted, sizeof wanted, "%s/x86_64-w64-mingw32-clang", root);
    ASSERT_TRUE(moltest_fake_program(wanted, "exit 0\n", c_driver, sizeof c_driver));
    snprintf(wanted, sizeof wanted, "%s/x86_64-w64-mingw32-clang++", root);
    ASSERT_TRUE(moltest_fake_program(wanted, "exit 0\n", cxx_driver, sizeof cxx_driver));

    toolchain chain = {0};
    snprintf(chain.name, sizeof chain.name, "x86_64-w64-mingw32-clang");
    snprintf(chain.path, sizeof chain.path, "%s", c_driver);

    probe_find_cxx_driver(&chain);
    EXPECT_STREQ(cxx_driver, chain.cxx_path);
}

/* And a bare name still pairs the way it always did: the prefix is optional,
   not required. */
MOLTEST(probe_still_pairs_a_versioned_driver_with_no_prefix) {
    char root[PICKUP_PATH_MAX];
    ASSERT_TRUE(moltest_temp_dir("pickup_versioned", root, sizeof root));

    char wanted[PICKUP_PATH_MAX];
    char c_driver[PICKUP_PATH_MAX];
    char cxx_driver[PICKUP_PATH_MAX];
    snprintf(wanted, sizeof wanted, "%s/gcc-12", root);
    ASSERT_TRUE(moltest_fake_program(wanted, "exit 0\n", c_driver, sizeof c_driver));
    snprintf(wanted, sizeof wanted, "%s/g++-12", root);
    ASSERT_TRUE(moltest_fake_program(wanted, "exit 0\n", cxx_driver, sizeof cxx_driver));

    toolchain chain = {0};
    snprintf(chain.name, sizeof chain.name, "gcc-12");
    snprintf(chain.path, sizeof chain.path, "%s", c_driver);

    probe_find_cxx_driver(&chain);
    EXPECT_STREQ(cxx_driver, chain.cxx_path);
}
