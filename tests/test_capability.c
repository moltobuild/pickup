#include <moltest.h>

#include <pickup/detect/capability.h>
#include <pickup/util/hash.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>

MOLTEST(capability_catalog_is_addressable_by_id) {
    size_t count = 0;
    const capability *catalog = capability_catalog(&count);
    ASSERT_NOT_NULL(catalog);
    EXPECT_TRUE(count > 0);

    size_t index = capability_index("attr_nodiscard");
    ASSERT_TRUE(index != SIZE_MAX);
    EXPECT_STREQ("attr_nodiscard", catalog[index].id);
    EXPECT_EQ(lang_c, catalog[index].lang);

    EXPECT_TRUE(capability_index("no_such_feature") == SIZE_MAX);
}

MOLTEST(capability_set_tracks_individual_features) {
    capability_set set = { 0 };
    size_t nodiscard = capability_index("attr_nodiscard");
    size_t typeof_id = capability_index("typeof");

    EXPECT_FALSE(capability_set_has(set, nodiscard));
    capability_set_add(&set, nodiscard);
    EXPECT_TRUE(capability_set_has(set, nodiscard));
    EXPECT_FALSE(capability_set_has(set, typeof_id));
}

MOLTEST(capability_set_contains_is_a_subset_test) {
    capability_set available = { 0 };
    capability_set required = { 0 };
    size_t a = capability_index("attr_nodiscard");
    size_t b = capability_index("typeof");

    capability_set_add(&available, a);
    capability_set_add(&required, a);
    EXPECT_TRUE(capability_set_contains(available, required));

    /* Requiring something absent must fail, whatever else is present. */
    capability_set_add(&required, b);
    EXPECT_FALSE(capability_set_contains(available, required));

    /* Requiring nothing is always satisfied. */
    capability_set none = { 0 };
    EXPECT_TRUE(capability_set_contains(none, none));
}

MOLTEST(capability_set_for_standard_groups_by_language) {
    capability_set c23 = capability_set_for_standard(lang_c, "c2x");
    EXPECT_TRUE(capability_set_has(c23, capability_index("attr_nodiscard")));
    /* A C++ feature must not leak into a C standard's set. */
    EXPECT_FALSE(capability_set_has(c23, capability_index("concepts")));

    capability_set unknown = capability_set_for_standard(lang_c, "c47");
    EXPECT_EQ(0, (int)unknown.bits);
}

MOLTEST(capability_catalog_fingerprint_is_stable_within_a_build) {
    /* Anything that stores feature bits compares this to decide whether they
       still mean what they meant, so it must not wander between calls. */
    EXPECT_TRUE(capability_catalog_fingerprint() == capability_catalog_fingerprint());
    EXPECT_TRUE(capability_catalog_fingerprint() != HASH_INIT);
}

/* The measurement this whole project is built on: gcc 9 accepts -std=c2x and
   does not implement [[nodiscard]]. Skipped where those compilers are absent,
   because the claim is about them, not about every machine. */
MOLTEST(capability_probing_separates_accepting_a_flag_from_implementing_it) {
    if (access("/usr/bin/gcc-9", X_OK) != 0 || access("/usr/bin/gcc-12", X_OK) != 0)
        SKIP("gcc-9 and gcc-12 are not both installed");

    EXPECT_TRUE(capability_accepts_standard("/usr/bin/gcc-9", lang_c, "c2x"));
    EXPECT_TRUE(capability_accepts_standard("/usr/bin/gcc-12", lang_c, "c2x"));

    capability_set old_features = capability_probe("/usr/bin/gcc-9", lang_c);
    capability_set new_features = capability_probe("/usr/bin/gcc-12", lang_c);
    size_t nodiscard = capability_index("attr_nodiscard");

    /* Both accept the flag; only one implements the attribute. A tool that
       tested the flag alone would recommend gcc-9 and be wrong. */
    EXPECT_FALSE(capability_set_has(old_features, nodiscard));
    EXPECT_TRUE(capability_set_has(new_features, nodiscard));

    /* Older features are present in both, so the probe is not just failing. */
    size_t static_assert_id = capability_index("static_assert");
    EXPECT_TRUE(capability_set_has(old_features, static_assert_id));
    EXPECT_TRUE(capability_set_has(new_features, static_assert_id));
}

MOLTEST(capability_rejects_a_standard_the_compiler_does_not_know) {
    if (access("/usr/bin/gcc-12", X_OK) != 0)
        SKIP("gcc-12 is not installed");
    EXPECT_FALSE(capability_accepts_standard("/usr/bin/gcc-12", lang_c, "c47"));
}

MOLTEST(capability_probing_a_missing_compiler_proves_nothing) {
    capability_set set = capability_probe("/nonexistent/compiler", lang_c);
    EXPECT_EQ(0, (int)set.bits);
    EXPECT_FALSE(capability_accepts_standard("/nonexistent/compiler", lang_c, "c11"));
}
