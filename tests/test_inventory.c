#include <moltest.h>

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
