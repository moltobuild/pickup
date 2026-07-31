#include <moltest.h>

#include <pickup/model/toolchain.h>

#include <string.h>

MOLTEST(toolchain_version_parses_partial_versions) {
    toolchain_version version;

    ASSERT_TRUE(toolchain_version_parse("12.3.0", &version));
    EXPECT_EQ(12, version.major);
    EXPECT_EQ(3, version.minor);
    EXPECT_EQ(0, version.patch);

    /* Compilers report versions with fewer components; missing ones are zero. */
    ASSERT_TRUE(toolchain_version_parse("14.0", &version));
    EXPECT_EQ(14, version.major);
    EXPECT_EQ(0, version.minor);

    ASSERT_TRUE(toolchain_version_parse("9", &version));
    EXPECT_EQ(9, version.major);

    EXPECT_FALSE(toolchain_version_parse("not-a-version", &version));
}

MOLTEST(toolchain_version_orders_by_component) {
    toolchain_version nine = { 9, 5, 0 };
    toolchain_version twelve = { 12, 3, 0 };
    toolchain_version twelve_patch = { 12, 3, 1 };

    EXPECT_TRUE(toolchain_version_compare(twelve, nine) > 0);
    EXPECT_TRUE(toolchain_version_compare(nine, twelve) < 0);
    EXPECT_EQ(0, toolchain_version_compare(twelve, twelve));
    EXPECT_TRUE(toolchain_version_compare(twelve_patch, twelve) > 0);
}

MOLTEST(toolchain_version_formats_all_components) {
    char text[32];
    toolchain_version_format((toolchain_version){ 12, 3, 0 }, text, sizeof text);
    EXPECT_STREQ("12.3.0", text);
}

MOLTEST(toolchain_vendor_names_round_trip) {
    EXPECT_STREQ("gcc", toolchain_vendor_name(vendor_gcc));
    EXPECT_STREQ("apple-clang", toolchain_vendor_name(vendor_apple_clang));
    EXPECT_EQ(vendor_clang, toolchain_vendor_parse("clang"));
    EXPECT_EQ(vendor_unknown, toolchain_vendor_parse("borland"));
}
