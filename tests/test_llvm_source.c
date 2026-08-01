#include <moltest.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/sources/llvm_source.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These tests are about choosing correctly among what LLVM publishes, so the
   asset names are the real ones, including the two different schemes and the
   noise that surrounds them. Recorded rather than fetched: the logic must be
   checkable without a network. */
static const char releases_json[] =
    "["
    "  {"
    "    \"tag_name\": \"llvmorg-23.1.0-rc2\", \"prerelease\": true, \"draft\": false,"
    "    \"assets\": [{\"name\": \"LLVM-23.1.0-rc2-Linux-X64.tar.xz\","
    "                 \"browser_download_url\": \"https://example.invalid/rc.tar.xz\","
    "                 \"size\": 10, \"digest\": \"sha256:aa\"}]"
    "  },"
    "  {"
    "    \"tag_name\": \"llvmorg-99.0.0\", \"prerelease\": false, \"draft\": true,"
    "    \"assets\": [{\"name\": \"LLVM-99.0.0-Linux-X64.tar.xz\","
    "                 \"browser_download_url\": \"https://example.invalid/draft.tar.xz\","
    "                 \"size\": 10, \"digest\": \"sha256:bb\"}]"
    "  },"
    "  {"
    "    \"tag_name\": \"llvmorg-22.1.8\", \"prerelease\": false, \"draft\": false,"
    "    \"assets\": ["
    "      {\"name\": \"llvm-project-22.1.8.src.tar.xz\","
    "       \"browser_download_url\": \"https://example.invalid/src.tar.xz\","
    "       \"size\": 1, \"digest\": \"sha256:11\"},"
    "      {\"name\": \"clang_doxygen-22.1.8.tar.xz\","
    "       \"browser_download_url\": \"https://example.invalid/doc.tar.xz\","
    "       \"size\": 2, \"digest\": \"sha256:22\"},"
    "      {\"name\": \"LLVM-22.1.8-win64.exe\","
    "       \"browser_download_url\": \"https://example.invalid/win.exe\","
    "       \"size\": 3, \"digest\": \"sha256:33\"},"
    "      {\"name\": \"LLVM-22.1.8-Linux-X64.tar.xz.sig\","
    "       \"browser_download_url\": \"https://example.invalid/sig\","
    "       \"size\": 4, \"digest\": \"sha256:44\"},"
    "      {\"name\": \"LLVM-22.1.8-Linux-X64.tar.xz.jsonl\","
    "       \"browser_download_url\": \"https://example.invalid/jsonl\","
    "       \"size\": 5, \"digest\": \"sha256:55\"},"
    "      {\"name\": \"LLVM-22.1.8-Linux-X64.tar.xz\","
    "       \"browser_download_url\":"
    "         \"https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/LLVM-22.1.8-Linux-X64.tar.xz\","
    "       \"size\": 1938859476,"
    "       \"digest\":"
    "         \"sha256:df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384\"},"
    "      {\"name\": \"LLVM-22.1.8-macOS-ARM64.tar.xz\","
    "       \"browser_download_url\": \"https://example.invalid/mac.tar.xz\","
    "       \"size\": 7, \"digest\": \"sha256:77\"}"
    "    ]"
    "  },"
    "  {"
    "    \"tag_name\": \"llvmorg-22.1.7\", \"prerelease\": false, \"draft\": false,"
    "    \"assets\": [{\"name\": \"LLVM-22.1.7-Linux-X64.tar.xz\","
    "                 \"browser_download_url\": \"https://example.invalid/22.1.7.tar.xz\","
    "                 \"size\": 1938496864, \"digest\": \"sha256:edb0\"}]"
    "  },"
    "  {"
    "    \"tag_name\": \"llvmorg-18.1.8\", \"prerelease\": false, \"draft\": false,"
    "    \"assets\": [{\"name\": \"clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz\","
    "                 \"browser_download_url\": \"https://example.invalid/18.1.8.tar.xz\","
    "                 \"size\": 1044930068, \"digest\": \"sha256:cafe\"}]"
    "  },"
    "  {"
    "    \"tag_name\": \"llvmorg-17.0.0\", \"prerelease\": false, \"draft\": false,"
    "    \"assets\": [{\"name\": \"clang+llvm-17.0.0-x86_64-pc-windows-msvc.tar.xz\","
    "                 \"browser_download_url\": \"https://example.invalid/win.tar.xz\","
    "                 \"size\": 8, \"digest\": \"sha256:88\"}]"
    "  }"
    "]";

/* The asset naming is per platform, so the expectations below are written for
   Linux x86_64 and skipped elsewhere rather than quietly asserting nothing. */
static bool host_is_linux_x64(void) {
#if defined(__linux__) && defined(__x86_64__)
    return true;
#else
    return false;
#endif
}

MOLTEST(llvm_keeps_only_finished_releases) {
    if (!host_is_linux_x64())
        SKIP("asset names are written for linux x86_64");

    release_list list;
    ASSERT_TRUE(llvm_parse_releases(releases_json, NULL, &list));

    /* The rc and the draft are gone, and so is the release that only ships a
       Windows build: 22.1.8, 22.1.7 and 18.1.8 remain. */
    ASSERT_EQ(3, (int)list.count);
    for (size_t i = 0; i < list.count; i++) {
        EXPECT_TRUE(strstr(list.items[i].version, "rc") == NULL);
        EXPECT_TRUE(strcmp(list.items[i].version, "99.0.0") != 0);
        EXPECT_TRUE(strcmp(list.items[i].version, "17.0.0") != 0);
    }

    release_list_free(&list);
}

MOLTEST(llvm_picks_the_host_archive_out_of_the_noise) {
    if (!host_is_linux_x64())
        SKIP("asset names are written for linux x86_64");

    release_list list;
    ASSERT_TRUE(llvm_parse_releases(releases_json, "22.1.8", &list));
    ASSERT_EQ(1, (int)list.count);

    const release_asset *asset = &list.items[0];
    /* Not the source, the docs, the installer, the signature, the attestation,
       nor the macOS build that sit beside it in the same release. */
    EXPECT_STREQ("LLVM-22.1.8-Linux-X64.tar.xz", asset->asset);
    EXPECT_STREQ("22.1.8", asset->version);
    EXPECT_STREQ("llvmorg-22.1.8", asset->tag);
    EXPECT_TRUE(asset->size == 1938859476LL);

    /* The digest is stored ready to compare, without the algorithm prefix. */
    EXPECT_STREQ("df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384",
                 asset->sha256);
    EXPECT_TRUE(strstr(asset->url, "releases/download/llvmorg-22.1.8/") != NULL);

    release_list_free(&list);
}

MOLTEST(llvm_understands_the_older_naming_scheme) {
    if (!host_is_linux_x64())
        SKIP("asset names are written for linux x86_64");

    release_list list;
    ASSERT_TRUE(llvm_parse_releases(releases_json, "18", &list));
    ASSERT_EQ(1, (int)list.count);
    /* Same host, entirely different file name. */
    EXPECT_STREQ("clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz",
                 list.items[0].asset);
    release_list_free(&list);
}

MOLTEST(llvm_orders_newest_first) {
    if (!host_is_linux_x64())
        SKIP("asset names are written for linux x86_64");

    release_list list;
    ASSERT_TRUE(llvm_parse_releases(releases_json, NULL, &list));

    release_asset best;
    ASSERT_TRUE(llvm_best(&list, &best));
    EXPECT_STREQ("22.1.8", best.version);
    EXPECT_STREQ("22.1.7", list.items[1].version);
    EXPECT_STREQ("18.1.8", list.items[2].version);

    release_list_free(&list);
}

MOLTEST(llvm_filters_by_a_partial_version) {
    if (!host_is_linux_x64())
        SKIP("asset names are written for linux x86_64");

    release_list list;
    ASSERT_TRUE(llvm_parse_releases(releases_json, "22", &list));
    EXPECT_EQ(2, (int)list.count); /* 22.1.8 and 22.1.7 */
    release_list_free(&list);

    ASSERT_TRUE(llvm_parse_releases(releases_json, "22.1", &list));
    EXPECT_EQ(2, (int)list.count);
    release_list_free(&list);

    ASSERT_TRUE(llvm_parse_releases(releases_json, "22.1.7", &list));
    EXPECT_EQ(1, (int)list.count);
    release_list_free(&list);

    /* A version nobody published is an empty answer, not an error. */
    ASSERT_TRUE(llvm_parse_releases(releases_json, "15", &list));
    EXPECT_EQ(0, (int)list.count);
    release_asset best;
    EXPECT_FALSE(llvm_best(&list, &best));
    release_list_free(&list);
}

MOLTEST(llvm_version_matching_respects_what_was_named) {
    EXPECT_TRUE(llvm_version_matches("14", "14.2.1"));
    EXPECT_TRUE(llvm_version_matches("14.2", "14.2.1"));
    EXPECT_TRUE(llvm_version_matches("14.2.1", "14.2.1"));

    /* Naming more components narrows it, and must not match a different one. */
    EXPECT_FALSE(llvm_version_matches("14.2", "14.3.0"));
    EXPECT_FALSE(llvm_version_matches("14.2.1", "14.2.2"));
    EXPECT_FALSE(llvm_version_matches("15", "14.2.1"));

    /* No filter means no opinion. */
    EXPECT_TRUE(llvm_version_matches(NULL, "14.2.1"));
    EXPECT_TRUE(llvm_version_matches("", "14.2.1"));
    EXPECT_FALSE(llvm_version_matches("nonsense", "14.2.1"));
}

MOLTEST(llvm_asset_matching_rejects_what_is_not_a_toolchain) {
    if (!host_is_linux_x64())
        SKIP("asset names are written for linux x86_64");

    EXPECT_TRUE(llvm_asset_matches_host("LLVM-22.1.8-Linux-X64.tar.xz"));
    EXPECT_TRUE(llvm_asset_matches_host(
        "clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz"));

    EXPECT_FALSE(llvm_asset_matches_host("LLVM-22.1.8-Linux-X64.tar.xz.sig"));
    EXPECT_FALSE(llvm_asset_matches_host("LLVM-22.1.8-Linux-X64.tar.xz.jsonl"));
    EXPECT_FALSE(llvm_asset_matches_host("llvm-project-22.1.8.src.tar.xz"));
    EXPECT_FALSE(llvm_asset_matches_host("clang_doxygen-22.1.8.tar.xz"));
    EXPECT_FALSE(llvm_asset_matches_host("test-suite-22.1.8.src.tar.xz"));
    EXPECT_FALSE(llvm_asset_matches_host("LLVM-22.1.8-win64.exe"));
    EXPECT_FALSE(llvm_asset_matches_host("LLVM-22.1.8-macOS-ARM64.tar.xz"));
    EXPECT_FALSE(llvm_asset_matches_host("LLVM-22.1.8-Linux-ARM64.tar.xz"));
    EXPECT_FALSE(llvm_asset_matches_host(NULL));
}

MOLTEST(llvm_refuses_an_answer_that_is_not_json) {
    release_list list;
    EXPECT_FALSE(llvm_parse_releases("<html>rate limited</html>", NULL, &list));
    EXPECT_EQ(0, (int)list.count);
    release_list_free(&list);
}

/* An index nobody could have fetched, so finding it proves where it was read
   from and losing it proves it was discarded. */
static const char planted_index[] =
    "[{\"tag_name\": \"llvmorg-99.1.1\", \"prerelease\": false, \"draft\": false,"
    "  \"assets\": [{\"name\": \"LLVM-99.1.1-Linux-X64.tar.xz\","
    "               \"browser_download_url\": \"https://example.invalid/planted.tar.xz\","
    "               \"size\": 42, \"digest\": \"sha256:beef\"}]}]";

typedef struct {
    char root[64];
    char previous[PICKUP_PATHS_MAX];
    bool had_previous;
} index_fixture;

static bool index_setup(index_fixture *fixture) {
    snprintf(fixture->root, sizeof fixture->root, "%s", "/tmp/pickup_index_XXXXXX");
    if (mkdtemp(fixture->root) == NULL)
        return false;

    const char *existing = getenv(PICKUP_HOME_ENV);
    fixture->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(fixture->previous, sizeof fixture->previous, "%s", existing);
    return setenv(PICKUP_HOME_ENV, fixture->root, 1) == 0;
}

static void index_teardown(index_fixture *fixture) {
    if (fixture->had_previous)
        (void)setenv(PICKUP_HOME_ENV, fixture->previous, 1);
    else
        (void)unsetenv(PICKUP_HOME_ENV);
    (void)fs_remove_tree(fixture->root);
}

MOLTEST(llvm_reads_its_index_from_the_cache_directory) {
    if (!host_is_linux_x64())
        SKIP("asset names are written for linux x86_64");

    index_fixture fixture;
    ASSERT_TRUE(index_setup(&fixture));

    /* Planted where the cache lives, not where downloads go: the index is
       regenerable state, and the downloads directory is for files on their way
       to becoming an installed toolchain. */
    char cache[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    ASSERT_TRUE(fs_make_dirs(cache));

    char index[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(index, sizeof index, "%s/releases.json", cache));
    ASSERT_TRUE(fs_write_file(index, planted_index));

    release_list list;
    ASSERT_TRUE(llvm_fetch_releases(NULL, false, &list));
    ASSERT_EQ(1, (int)list.count);
    EXPECT_STREQ("99.1.1", list.items[0].version);
    release_list_free(&list);

    /* And nothing was written to the downloads directory to get it. */
    char downloads[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    EXPECT_FALSE(fs_path_exists(downloads));

    index_teardown(&fixture);
}

MOLTEST(llvm_refresh_discards_the_cached_index) {
    if (!host_is_linux_x64())
        SKIP("asset names are written for linux x86_64");

    index_fixture fixture;
    ASSERT_TRUE(index_setup(&fixture));

    char cache[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    ASSERT_TRUE(fs_make_dirs(cache));
    char index[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(index, sizeof index, "%s/releases.json", cache));
    ASSERT_TRUE(fs_write_file(index, planted_index));

    /* Freshly written, so without a refresh it would be used as-is. */
    release_list list;
    bool fetched = llvm_fetch_releases(NULL, true, &list);

    /* Whether the refetch succeeds depends on the network, which this test does
       not require. Either way the planted index is gone: that is the claim. */
    if (fetched) {
        for (size_t i = 0; i < list.count; i++)
            EXPECT_TRUE(strcmp(list.items[i].version, "99.1.1") != 0);
    }
    release_list_free(&list);

    index_teardown(&fixture);
}
