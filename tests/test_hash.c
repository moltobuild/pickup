#include <moltest.h>

#include <pickup/util/hash.h>

MOLTEST(hash_is_deterministic) {
    EXPECT_TRUE(hash_add(HASH_INIT, "gcc-12") == hash_add(HASH_INIT, "gcc-12"));
}

MOLTEST(hash_separates_the_items_it_folds_in) {
    /* Without a terminator between items these would collide, and a cache
       fingerprint that collides is a cache that is trusted when it should not
       be. */
    unsigned long long split = hash_add(hash_add(HASH_INIT, "ab"), "c");
    unsigned long long other = hash_add(hash_add(HASH_INIT, "a"), "bc");
    EXPECT_TRUE(split != other);
}

MOLTEST(hash_changes_with_the_text) {
    EXPECT_TRUE(hash_add(HASH_INIT, "gcc-12") != hash_add(HASH_INIT, "gcc-11"));
    EXPECT_TRUE(hash_add(HASH_INIT, "") != HASH_INIT);
}

MOLTEST(hash_depends_on_the_order_of_the_items) {
    unsigned long long forward = hash_add(hash_add(HASH_INIT, "gcc"), "clang");
    unsigned long long backward = hash_add(hash_add(HASH_INIT, "clang"), "gcc");
    EXPECT_TRUE(forward != backward);
}
