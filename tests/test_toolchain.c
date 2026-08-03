#include <moltest.h>

#include <stdio.h>

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

/*
 * Identity: what a toolchain is called, as opposed to what a link to it
 * happens to be named.
 */

MOLTEST(toolchain_id_is_the_vendor_and_version) {
    toolchain chain = { .vendor = vendor_gcc, .version = { 12, 3, 0 } };
    snprintf(chain.target, sizeof chain.target, "%s", "x86_64-linux-gnu");
    toolchain_make_id(&chain);
    EXPECT_STREQ("gcc@12.3.0", chain.id);

    toolchain clang = { .vendor = vendor_clang, .version = { 22, 1, 8 } };
    snprintf(clang.target, sizeof clang.target, "%s", "x86_64-unknown-linux-gnu");
    toolchain_make_id(&clang);
    /* "unknown" and "pc" are placeholders every triple carries; neither says
       which toolchain this is, so neither belongs in its name. */
    EXPECT_STREQ("clang@22.1.8", clang.id);
}

MOLTEST(toolchain_id_marks_a_target_that_is_not_the_ordinary_one) {
    toolchain chain = { .vendor = vendor_gcc, .version = { 12, 3, 0 } };
    snprintf(chain.target, sizeof chain.target, "%s", "x86_64-conda-linux-gnu");
    toolchain_make_id(&chain);
    /* The whole point: this must not collide with the system's own 12.3.0. */
    EXPECT_STREQ("gcc@12.3.0-conda", chain.id);
}

MOLTEST(toolchain_tag_comes_from_the_target_alone) {
    char tag[64];

    /* Derived from the triple itself, never by comparing toolchains with each
       other: a name that changed because something unrelated was installed
       would be no name at all. */
    toolchain_target_tag("x86_64-conda-linux-gnu", tag, sizeof tag);
    EXPECT_STREQ("conda", tag);
    toolchain_target_tag("x86_64-linux-musl", tag, sizeof tag);
    EXPECT_STREQ("musl", tag);

    toolchain_target_tag("x86_64-linux-gnu", tag, sizeof tag);
    EXPECT_STREQ("", tag);
    toolchain_target_tag("x86_64-pc-linux-gnu", tag, sizeof tag);
    EXPECT_STREQ("", tag);
    toolchain_target_tag("", tag, sizeof tag);
    EXPECT_STREQ("", tag);
    toolchain_target_tag(NULL, tag, sizeof tag);
    EXPECT_STREQ("", tag);
}

MOLTEST(toolchain_answers_to_the_shorter_forms_a_person_types) {
    toolchain chain = { .vendor = vendor_gcc, .version = { 12, 3, 0 } };
    snprintf(chain.target, sizeof chain.target, "%s", "x86_64-linux-gnu");
    snprintf(chain.path, sizeof chain.path, "%s", "/usr/bin/gcc-12");
    snprintf(chain.name, sizeof chain.name, "%s", "gcc-12");
    toolchain_make_id(&chain);

    EXPECT_TRUE(toolchain_matches(&chain, "gcc@12.3.0"));
    EXPECT_TRUE(toolchain_matches(&chain, "gcc@12.3"));
    EXPECT_TRUE(toolchain_matches(&chain, "gcc@12"));
    EXPECT_TRUE(toolchain_matches(&chain, "gcc@latest"));
    EXPECT_TRUE(toolchain_matches(&chain, "gcc"));
    /* The path and the binary's own name still work: both are things someone
       may have in front of them. */
    EXPECT_TRUE(toolchain_matches(&chain, "/usr/bin/gcc-12"));
    EXPECT_TRUE(toolchain_matches(&chain, "gcc-12"));

    EXPECT_FALSE(toolchain_matches(&chain, "gcc@11"));
    EXPECT_FALSE(toolchain_matches(&chain, "gcc@12.4"));
    EXPECT_FALSE(toolchain_matches(&chain, "clang@12.3.0"));
    EXPECT_FALSE(toolchain_matches(&chain, ""));
    EXPECT_FALSE(toolchain_matches(&chain, NULL));
}

MOLTEST(toolchain_tells_two_of_the_same_version_apart_by_their_tag) {
    toolchain system = { .vendor = vendor_gcc, .version = { 12, 3, 0 } };
    snprintf(system.target, sizeof system.target, "%s", "x86_64-linux-gnu");
    toolchain_make_id(&system);

    toolchain conda = { .vendor = vendor_gcc, .version = { 12, 3, 0 } };
    snprintf(conda.target, sizeof conda.target, "%s", "x86_64-conda-linux-gnu");
    toolchain_make_id(&conda);

    /* Naming the tag picks exactly one of them. */
    EXPECT_TRUE(toolchain_matches(&conda, "gcc@12.3.0-conda"));
    EXPECT_FALSE(toolchain_matches(&system, "gcc@12.3.0-conda"));

    /* Leaving it off matches both, which is what makes the query ambiguous
       rather than wrong. */
    EXPECT_TRUE(toolchain_matches(&conda, "gcc@12.3.0"));
    EXPECT_TRUE(toolchain_matches(&system, "gcc@12.3.0"));
}

MOLTEST(toolchain_names_both_sources) {
    EXPECT_STREQ("system", toolchain_source_name(toolchain_source_system));
    EXPECT_STREQ("pickup", toolchain_source_name(toolchain_source_pickup));
}
