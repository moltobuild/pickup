#include <moltest.h>

#include <pickup/detect/scanner.h>
#include <pickup/services/cache_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/inventory_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Point the cache at a throwaway directory so the tests never touch the real
   one, and remember what to restore. */
typedef struct {
    char root[64];
    char previous[4096];
    bool had_previous;
} cache_fixture;

static bool fixture_setup(cache_fixture *fixture) {
    snprintf(fixture->root, sizeof fixture->root, "%s", "/tmp/pickup_cache_XXXXXX");
    if (mkdtemp(fixture->root) == NULL)
        return false;

    const char *existing = getenv("XDG_CACHE_HOME");
    fixture->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(fixture->previous, sizeof fixture->previous, "%s", existing);
    return setenv("XDG_CACHE_HOME", fixture->root, 1) == 0;
}

static void fixture_teardown(cache_fixture *fixture) {
    if (fixture->had_previous)
        (void)setenv("XDG_CACHE_HOME", fixture->previous, 1);
    else
        (void)unsetenv("XDG_CACHE_HOME");
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", fixture->root);
    (void)system(cmd);
}

/* The candidate list a load has to be checked against. */
static bool collect_candidates(str_list *out) {
    str_list_init(out);
    return scanner_collect(getenv("PATH"), out);
}

MOLTEST(cache_round_trips_an_inventory) {
    cache_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    str_list candidates;
    ASSERT_TRUE(collect_candidates(&candidates));

    inventory discovered;
    ASSERT_TRUE(inventory_discover(&discovered));
    ASSERT_TRUE(cache_store(&candidates, &discovered));

    inventory restored;
    ASSERT_TRUE(cache_load(&candidates, &restored));
    ASSERT_EQ(discovered.count, restored.count);

    /* Everything probed must survive the round trip: a cache that loses a
       feature would have a compiler rejected for something it can do. */
    for (size_t i = 0; i < discovered.count; i++) {
        EXPECT_STREQ(discovered.items[i].path, restored.items[i].path);
        EXPECT_STREQ(discovered.items[i].name, restored.items[i].name);
        EXPECT_STREQ(discovered.items[i].cxx_path, restored.items[i].cxx_path);
        EXPECT_STREQ(discovered.items[i].target, restored.items[i].target);
        EXPECT_EQ(discovered.items[i].vendor, restored.items[i].vendor);
        EXPECT_EQ(0, toolchain_version_compare(discovered.items[i].version,
                                               restored.items[i].version));
        EXPECT_TRUE(discovered.items[i].c_features.bits
                    == restored.items[i].c_features.bits);
        EXPECT_TRUE(discovered.items[i].cxx_features.bits
                    == restored.items[i].cxx_features.bits);
    }

    inventory_free(&discovered);
    inventory_free(&restored);
    str_list_free(&candidates);
    fixture_teardown(&fixture);
}

MOLTEST(cache_is_rejected_when_the_candidate_set_changed) {
    cache_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    str_list candidates;
    ASSERT_TRUE(collect_candidates(&candidates));
    inventory discovered;
    ASSERT_TRUE(inventory_discover(&discovered));
    ASSERT_TRUE(cache_store(&candidates, &discovered));

    /* A compiler installed since the last scan is not in the cache, and the
       cache cannot know anything about one it never probed. */
    ASSERT_TRUE(str_list_push(&candidates, "/usr/bin/newly-installed-gcc"));
    inventory restored;
    EXPECT_FALSE(cache_load(&candidates, &restored));

    inventory_free(&discovered);
    str_list_free(&candidates);
    fixture_teardown(&fixture);
}

MOLTEST(cache_is_rejected_when_a_binary_changed) {
    cache_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    /* One entry describing a file we control, so its signature can be moved. */
    char binary[128];
    snprintf(binary, sizeof binary, "%s/fake-cc", fixture.root);
    ASSERT_TRUE(fs_write_file(binary, "#!/bin/sh\n"));

    str_list candidates;
    str_list_init(&candidates);
    ASSERT_TRUE(str_list_push(&candidates, binary));

    inventory list;
    memset(&list, 0, sizeof list);
    toolchain chain;
    memset(&chain, 0, sizeof chain);
    snprintf(chain.path, sizeof chain.path, "%s", binary);
    snprintf(chain.name, sizeof chain.name, "%s", "fake-cc");
    chain.vendor = vendor_gcc;
    chain.version = (toolchain_version){ 1, 0, 0 };
    ASSERT_TRUE(inventory_append(&list, &chain));
    ASSERT_TRUE(cache_store(&candidates, &list));

    /* Unchanged, the cache is good. */
    inventory restored;
    EXPECT_TRUE(cache_load(&candidates, &restored));
    inventory_free(&restored);

    /* Replacing the binary invalidates everything probed about it. */
    ASSERT_TRUE(fs_write_file(binary, "#!/bin/sh\n# a different compiler\n"));
    EXPECT_FALSE(cache_load(&candidates, &restored));

    inventory_free(&list);
    str_list_free(&candidates);
    fixture_teardown(&fixture);
}

MOLTEST(cache_discards_a_corrupt_file) {
    cache_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    str_list candidates;
    ASSERT_TRUE(collect_candidates(&candidates));
    inventory discovered;
    ASSERT_TRUE(inventory_discover(&discovered));
    ASSERT_TRUE(cache_store(&candidates, &discovered));

    char path[256];
    snprintf(path, sizeof path, "%s/pickup/toolchains", fixture.root);
    ASSERT_TRUE(fs_write_file(path, "this is not a cache\n"));

    /* Garbage is discarded, never trusted: rescanning is always correct. */
    inventory restored;
    EXPECT_FALSE(cache_load(&candidates, &restored));

    inventory_free(&discovered);
    str_list_free(&candidates);
    fixture_teardown(&fixture);
}

MOLTEST(cache_load_without_a_cache_simply_fails) {
    cache_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    str_list candidates;
    ASSERT_TRUE(collect_candidates(&candidates));

    inventory restored;
    EXPECT_FALSE(cache_load(&candidates, &restored));
    EXPECT_EQ(0, restored.count);

    str_list_free(&candidates);
    fixture_teardown(&fixture);
}
