#include <moltest.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/sources/registry_source.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Answers recorded from the registry, trimmed to what is read back. */

static const char catalogue_json[] =
    "{\"kind\":\"toolchain\",\"entries\":["
    " {\"name\":\"clang\",\"latest_version\":\"19.1.6\",\"versions\":1,"
    "  \"targets\":[\"linux-x86_64\"],\"published_at\":\"2026-08-05T19:41:45Z\"},"
    " {\"name\":\"gcc\",\"latest_version\":\"16.1.0\",\"versions\":1,"
    "  \"targets\":[\"linux-x86_64\"],\"published_at\":\"2026-08-06T16:56:52Z\"}]}";

static const char releases_json[] =
    "{\"kind\":\"toolchain\",\"name\":\"gcc\",\"releases\":[{"
    " \"kind\":\"toolchain\",\"name\":\"gcc\",\"version\":\"16.1.0\",\"targets\":[{"
    "  \"kind\":\"toolchain\",\"name\":\"gcc\",\"version\":\"16.1.0\","
    "  \"target\":\"linux-x86_64\",\"format\":\"tar.zst\","
    "  \"checksum\":\"062ae43f2c002b8149b62ee52862f15504a4e3318852aa38d0a0700c5025bbfc\","
    "  \"size_bytes\":68711760,\"yanked\":false,"
    "  \"published_at\":\"2026-08-06T16:56:52Z\","
    "  \"metadata\":{\"provides\":[\"concepts\",\"spaceship\"],"
    "   \"toolchain\":{\"vendor\":\"gcc\",\"triple\":\"x86_64-conda-linux-gnu\"},"
    "   \"about\":{\"description\":\"GNU C and C++ compiler\"}},"
    "  \"download_url\":\"https://example.invalid/gcc.tar.zst\"}]}]}";

static const char tool_json[] =
    "{\"kind\":\"tool\",\"name\":\"clang-format\",\"releases\":[{"
    " \"kind\":\"tool\",\"name\":\"clang-format\",\"version\":\"19.1.6\",\"targets\":[{"
    "  \"kind\":\"tool\",\"name\":\"clang-format\",\"version\":\"19.1.6\","
    "  \"target\":\"linux-x86_64\",\"format\":\"tar.zst\",\"checksum\":\"aa\","
    "  \"size_bytes\":1329462,\"yanked\":false,"
    "  \"metadata\":{\"tool\":{\"kind\":\"formatter\",\"binary\":\"bin/clang-format\"}},"
    "  \"download_url\":\"https://example.invalid/cf.tar.zst\"}]}]}";

static const char search_json[] =
    "{\"query\":\"clang\",\"results\":["
    " {\"kind\":\"toolchain\",\"name\":\"clang\",\"latest_version\":\"19.1.6\","
    "  \"targets\":1,\"published_at\":\"2026-08-05T19:41:45Z\"},"
    " {\"kind\":\"tool\",\"name\":\"clang-format\",\"latest_version\":\"19.1.6\","
    "  \"targets\":1,\"published_at\":\"2026-08-05T15:26:03Z\"}]}";

static const char error_json[] =
    "{\"error\":\"not_found\",\"message\":\"no toolchain named 'nope'\"}";

/* --- what a catalogue says --- */

MOLTEST(registry_reads_a_catalogue) {
    registry_entry_list list;
    ASSERT_TRUE(registry_parse_catalogue(catalogue_json, &list));
    ASSERT_EQ(2, (int)list.count);

    EXPECT_STREQ("clang", list.items[0].name);
    EXPECT_STREQ("19.1.6", list.items[0].latest_version);
    EXPECT_EQ(1, (int)list.items[0].versions);
    ASSERT_EQ(1, (int)list.items[0].target_count);
    EXPECT_STREQ("linux-x86_64", list.items[0].targets[0]);
    EXPECT_EQ(registry_kind_toolchain, list.items[0].kind);
    EXPECT_STREQ("gcc", list.items[1].name);

    registry_entry_list_free(&list);
}

MOLTEST(registry_reads_a_search_answer) {
    registry_entry_list list;
    ASSERT_TRUE(registry_parse_search(search_json, &list));
    ASSERT_EQ(2, (int)list.count);

    /* A search row carries a count of targets rather than their names, so the
       row says what exists without pretending to say what can be installed. */
    EXPECT_EQ(0, (int)list.items[0].target_count);
    EXPECT_EQ(registry_kind_toolchain, list.items[0].kind);
    EXPECT_EQ(registry_kind_tool, list.items[1].kind);
    EXPECT_STREQ("clang-format", list.items[1].name);

    registry_entry_list_free(&list);
}

/* --- what a release says --- */

MOLTEST(registry_reads_an_artifact_whole) {
    registry_artifact_list list;
    ASSERT_TRUE(registry_parse_releases(releases_json, NULL, NULL, &list));
    ASSERT_EQ(1, (int)list.count);

    const registry_artifact *artifact = &list.items[0];
    EXPECT_STREQ("gcc", artifact->name);
    EXPECT_STREQ("16.1.0", artifact->version);
    EXPECT_STREQ("linux-x86_64", artifact->target);
    EXPECT_STREQ("tar.zst", artifact->format);
    EXPECT_STREQ("062ae43f2c002b8149b62ee52862f15504a4e3318852aa38d0a0700c5025bbfc",
                 artifact->checksum);
    EXPECT_EQ(68711760LL, artifact->size_bytes);
    EXPECT_FALSE(artifact->yanked);
    EXPECT_STREQ("https://example.invalid/gcc.tar.zst", artifact->download_url);

    /* Read and carried, so a person can see it before installing. */
    EXPECT_STREQ("gcc", artifact->vendor);
    EXPECT_STREQ("x86_64-conda-linux-gnu", artifact->triple);
    EXPECT_STREQ("GNU C and C++ compiler", artifact->description);
    ASSERT_EQ(2, (int)artifact->provides_count);
    EXPECT_STREQ("spaceship", artifact->provides[1]);

    registry_artifact_list_free(&list);
}

MOLTEST(registry_reads_the_binary_a_tool_names) {
    registry_artifact_list list;
    ASSERT_TRUE(registry_parse_releases(tool_json, NULL, NULL, &list));
    ASSERT_EQ(1, (int)list.count);
    EXPECT_EQ(registry_kind_tool, list.items[0].kind);
    EXPECT_STREQ("bin/clang-format", list.items[0].binary);
    registry_artifact_list_free(&list);
}

MOLTEST(registry_keeps_only_the_target_asked_for) {
    registry_artifact_list list;
    ASSERT_TRUE(registry_parse_releases(releases_json, NULL, "linux-aarch64", &list));

    /* Nothing is published for that machine. It is an answer, not a failure:
       the parse succeeds and the list is empty. */
    EXPECT_EQ(0, (int)list.count);
    registry_artifact_list_free(&list);
}

MOLTEST(registry_drops_an_artifact_missing_what_it_must_have) {
    /* No checksum. Verifying is not optional, so this cannot be carried to the
       point where something decides whether to check it. */
    static const char no_digest[] =
        "{\"kind\":\"toolchain\",\"name\":\"clang\",\"releases\":[{"
        " \"version\":\"19.1.6\",\"targets\":[{"
        "  \"name\":\"clang\",\"version\":\"19.1.6\",\"target\":\"linux-x86_64\","
        "  \"format\":\"tar.zst\",\"size_bytes\":1,"
        "  \"download_url\":\"https://example.invalid/x\"}]}]}";

    registry_artifact_list list;
    ASSERT_TRUE(registry_parse_releases(no_digest, NULL, NULL, &list));
    EXPECT_EQ(0, (int)list.count);
    registry_artifact_list_free(&list);
}

MOLTEST(registry_reads_a_single_artifact_document) {
    static const char one[] =
        "{\"kind\":\"tool\",\"name\":\"clang-tidy\",\"version\":\"19.1.6\","
        " \"target\":\"linux-x86_64\",\"format\":\"tar.zst\",\"checksum\":\"bf\","
        " \"size_bytes\":24599140,\"yanked\":false,"
        " \"download_url\":\"https://example.invalid/ct.tar.zst\"}";

    registry_artifact artifact;
    ASSERT_TRUE(registry_parse_artifact(one, &artifact));
    EXPECT_EQ(registry_kind_tool, artifact.kind);
    EXPECT_STREQ("clang-tidy", artifact.name);
    EXPECT_EQ(24599140LL, artifact.size_bytes);
}

MOLTEST(registry_reads_the_error_code) {
    char code[64] = "";
    char message[256] = "";
    ASSERT_TRUE(registry_parse_error(error_json, code, sizeof code,
                                     message, sizeof message));
    EXPECT_STREQ("not_found", code);
    EXPECT_STREQ("no toolchain named 'nope'", message);
}

MOLTEST(registry_refuses_to_read_something_that_is_not_json) {
    registry_entry_list list;
    EXPECT_FALSE(registry_parse_catalogue("<html>404</html>", &list));
}

/* --- choosing a version --- */

MOLTEST(registry_matches_the_components_a_filter_names) {
    EXPECT_TRUE(registry_version_matches(NULL, "19.1.6"));
    EXPECT_TRUE(registry_version_matches("", "19.1.6"));
    EXPECT_TRUE(registry_version_matches("19", "19.1.6"));
    EXPECT_TRUE(registry_version_matches("19.1", "19.1.6"));
    EXPECT_TRUE(registry_version_matches("19.1.6", "19.1.6"));
    EXPECT_FALSE(registry_version_matches("19.2", "19.1.6"));
    EXPECT_FALSE(registry_version_matches("20", "19.1.6"));
}

MOLTEST(registry_knows_when_a_filter_names_one_whole_version) {
    EXPECT_TRUE(registry_version_is_exact("19.1.6"));
    EXPECT_FALSE(registry_version_is_exact("19"));
    EXPECT_FALSE(registry_version_is_exact("19.1"));
    EXPECT_FALSE(registry_version_is_exact(""));
    EXPECT_FALSE(registry_version_is_exact(NULL));
}

/* Two releases of one name, newest first, the newest withdrawn. */
static void two_releases(registry_artifact_list *list) {
    registry_artifact_list_init(list);

    registry_artifact newer = { 0 };
    (void)fs_format_path(newer.name, sizeof newer.name, "%s", "clang");
    (void)fs_format_path(newer.version, sizeof newer.version, "%s", "19.1.7");
    newer.yanked = true;

    registry_artifact older = { 0 };
    (void)fs_format_path(older.name, sizeof older.name, "%s", "clang");
    (void)fs_format_path(older.version, sizeof older.version, "%s", "19.1.6");

    /* Pushed through the parser's own path so the test cannot drift from it. */
    static const char json[] =
        "{\"kind\":\"toolchain\",\"name\":\"clang\",\"releases\":[{"
        " \"version\":\"19.1.7\",\"targets\":[{"
        "  \"name\":\"clang\",\"version\":\"19.1.7\",\"target\":\"linux-x86_64\","
        "  \"format\":\"tar.zst\",\"checksum\":\"a\",\"size_bytes\":1,"
        "  \"yanked\":true,\"download_url\":\"https://example.invalid/a\"}]},{"
        " \"version\":\"19.1.6\",\"targets\":[{"
        "  \"name\":\"clang\",\"version\":\"19.1.6\",\"target\":\"linux-x86_64\","
        "  \"format\":\"tar.zst\",\"checksum\":\"b\",\"size_bytes\":1,"
        "  \"yanked\":false,\"download_url\":\"https://example.invalid/b\"}]}]}";
    (void)registry_parse_releases(json, NULL, NULL, list);
}

MOLTEST(registry_skips_what_was_withdrawn) {
    registry_artifact_list list;
    two_releases(&list);
    ASSERT_EQ(2, (int)list.count);

    registry_artifact chosen;
    ASSERT_TRUE(registry_select(&list, NULL, &chosen));
    EXPECT_STREQ("19.1.6", chosen.version);

    registry_artifact_list_free(&list);
}

MOLTEST(registry_installs_what_was_withdrawn_when_it_is_named) {
    registry_artifact_list list;
    two_releases(&list);

    /* Withdrawn means "not for new builds", not "gone". Naming the whole
       version is the only way to ask for it, and asking is allowed. */
    registry_artifact chosen;
    ASSERT_TRUE(registry_select(&list, "19.1.7", &chosen));
    EXPECT_STREQ("19.1.7", chosen.version);
    EXPECT_TRUE(chosen.yanked);

    /* Naming the family is not naming the version: "19.1" would match the
       withdrawn one first, and skips it to the one still offered. */
    ASSERT_TRUE(registry_select(&list, "19.1", &chosen));
    EXPECT_STREQ("19.1.6", chosen.version);

    registry_artifact_list_free(&list);
}

MOLTEST(registry_answers_that_nothing_matches) {
    registry_artifact_list list;
    two_releases(&list);

    registry_artifact chosen;
    EXPECT_FALSE(registry_select(&list, "22", &chosen));

    registry_artifact_list_free(&list);
}

/* --- names --- */

MOLTEST(registry_refuses_a_name_that_would_leave_the_cache) {
    EXPECT_TRUE(registry_name_is_simple("clang"));
    EXPECT_TRUE(registry_name_is_simple("clang-format"));
    EXPECT_TRUE(registry_name_is_simple("gcc"));

    EXPECT_FALSE(registry_name_is_simple(".."));
    EXPECT_FALSE(registry_name_is_simple("../../etc/passwd"));
    EXPECT_FALSE(registry_name_is_simple("a/b"));
    EXPECT_FALSE(registry_name_is_simple(""));
    EXPECT_FALSE(registry_name_is_simple(NULL));
    EXPECT_FALSE(registry_name_is_simple("a b"));
}

MOLTEST(registry_names_the_paths_of_its_kinds) {
    EXPECT_STREQ("toolchains", registry_kind_path(registry_kind_toolchain));
    EXPECT_STREQ("tools", registry_kind_path(registry_kind_tool));
    EXPECT_STREQ("packages", registry_kind_path(registry_kind_package));
    EXPECT_STREQ("toolchain", registry_kind_name(registry_kind_toolchain));
}

/* --- where it installs from --- */

typedef struct {
    char root[64];
    char previous_home[PICKUP_PATHS_MAX];
    bool had_home;
    char previous_url[REGISTRY_URL_MAX];
    bool had_url;
} home_fixture;

static bool home_setup(home_fixture *fixture) {
    snprintf(fixture->root, sizeof fixture->root, "%s", "/tmp/pickup_registry_XXXXXX");
    if (mkdtemp(fixture->root) == NULL)
        return false;

    const char *home = getenv(PICKUP_HOME_ENV);
    fixture->had_home = home != NULL;
    if (home != NULL)
        snprintf(fixture->previous_home, sizeof fixture->previous_home, "%s", home);

    const char *url = getenv(REGISTRY_URL_ENV);
    fixture->had_url = url != NULL;
    if (url != NULL)
        snprintf(fixture->previous_url, sizeof fixture->previous_url, "%s", url);
    (void)unsetenv(REGISTRY_URL_ENV);

    return setenv(PICKUP_HOME_ENV, fixture->root, 1) == 0;
}

static void home_teardown(home_fixture *fixture) {
    if (fixture->had_home)
        (void)setenv(PICKUP_HOME_ENV, fixture->previous_home, 1);
    else
        (void)unsetenv(PICKUP_HOME_ENV);

    if (fixture->had_url)
        (void)setenv(REGISTRY_URL_ENV, fixture->previous_url, 1);
    else
        (void)unsetenv(REGISTRY_URL_ENV);

    (void)fs_remove_tree(fixture->root);
}

static bool write_config(const char *contents) {
    char home[PICKUP_PATHS_MAX];
    char path[PICKUP_PATHS_MAX];
    return paths_home(home, sizeof home) && fs_make_dirs(home)
        && fs_format_path(path, sizeof path, "%s/config.toml", home)
        && fs_write_file(path, contents);
}

MOLTEST(registry_falls_back_to_the_built_in_url) {
    home_fixture fixture;
    ASSERT_TRUE(home_setup(&fixture));

    char base[REGISTRY_URL_MAX];
    ASSERT_TRUE(registry_base_url(base, sizeof base));
    EXPECT_STREQ(REGISTRY_DEFAULT_URL, base);

    home_teardown(&fixture);
}

MOLTEST(registry_takes_the_url_the_file_records) {
    home_fixture fixture;
    ASSERT_TRUE(home_setup(&fixture));
    ASSERT_TRUE(write_config("default = \"gcc@12\"\nregistry = \"https://mine.invalid\"\n"));

    char base[REGISTRY_URL_MAX];
    ASSERT_TRUE(registry_base_url(base, sizeof base));
    EXPECT_STREQ("https://mine.invalid", base);

    home_teardown(&fixture);
}

MOLTEST(registry_lets_the_environment_win) {
    home_fixture fixture;
    ASSERT_TRUE(home_setup(&fixture));
    ASSERT_TRUE(write_config("registry = \"https://file.invalid\"\n"));
    ASSERT_EQ(0, setenv(REGISTRY_URL_ENV, "https://env.invalid/", 1));

    char base[REGISTRY_URL_MAX];
    ASSERT_TRUE(registry_base_url(base, sizeof base));

    /* The environment wins, and the trailing slash goes: every caller composes
       "<base>/v1/…" the same way. */
    EXPECT_STREQ("https://env.invalid", base);

    home_teardown(&fixture);
}

/* --- the cached index --- */

/* An index nobody could have fetched, so finding it proves where it was read
   from and losing it proves it was discarded. */
static const char planted_catalogue[] =
    "{\"kind\":\"toolchain\",\"entries\":[{\"name\":\"planted\","
    " \"latest_version\":\"99.1.1\",\"versions\":1,\"targets\":[\"linux-x86_64\"]}]}";

static bool plant(const char *filename, const char *contents) {
    char cache[PICKUP_PATHS_MAX];
    char path[PICKUP_PATHS_MAX];
    return paths_cache(cache, sizeof cache) && fs_make_dirs(cache)
        && fs_format_path(path, sizeof path, "%s/%s", cache, filename)
        && fs_write_file(path, contents);
}

MOLTEST(registry_reads_its_catalogue_from_the_cache_directory) {
    home_fixture fixture;
    ASSERT_TRUE(home_setup(&fixture));
    ASSERT_TRUE(plant("registry-toolchains.json", planted_catalogue));

    registry_entry_list list;
    ASSERT_TRUE(registry_fetch_catalogue(registry_kind_toolchain, false, &list));
    ASSERT_EQ(1, (int)list.count);
    EXPECT_STREQ("planted", list.items[0].name);
    registry_entry_list_free(&list);

    /* And nothing was written to the downloads directory to get it. */
    char downloads[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_downloads(downloads, sizeof downloads));
    EXPECT_FALSE(fs_path_exists(downloads));

    home_teardown(&fixture);
}

MOLTEST(registry_finds_which_catalogue_publishes_a_name) {
    home_fixture fixture;
    ASSERT_TRUE(home_setup(&fixture));
    ASSERT_TRUE(plant("registry-toolchains.json", catalogue_json));
    ASSERT_TRUE(plant("registry-tools.json",
                      "{\"kind\":\"tool\",\"entries\":[{\"name\":\"clang-format\","
                      " \"latest_version\":\"19.1.6\",\"versions\":1,\"targets\":[]}]}"));

    registry_entry found;
    ASSERT_TRUE(registry_find("gcc", false, &found));
    EXPECT_EQ(registry_kind_toolchain, found.kind);
    EXPECT_STREQ("16.1.0", found.latest_version);

    ASSERT_TRUE(registry_find("clang-format", false, &found));
    EXPECT_EQ(registry_kind_tool, found.kind);

    home_teardown(&fixture);
}

MOLTEST(registry_discards_a_cached_answer_that_no_longer_parses) {
    home_fixture fixture;
    ASSERT_TRUE(home_setup(&fixture));
    ASSERT_TRUE(plant("registry-toolchains.json", "not json at all"));

    registry_entry_list list;
    EXPECT_FALSE(registry_fetch_catalogue(registry_kind_toolchain, false, &list));

    /* Dropped rather than kept, so the next run fetches a fresh one instead of
       failing again on the same rubbish. */
    char cache[PICKUP_PATHS_MAX];
    char path[PICKUP_PATHS_MAX];
    ASSERT_TRUE(paths_cache(cache, sizeof cache));
    ASSERT_TRUE(fs_format_path(path, sizeof path, "%s/registry-toolchains.json", cache));
    EXPECT_FALSE(fs_path_exists(path));

    home_teardown(&fixture);
}

MOLTEST(registry_refuses_to_fetch_a_name_that_would_leave_the_cache) {
    home_fixture fixture;
    ASSERT_TRUE(home_setup(&fixture));

    /* Refused before anything is composed, so no URL and no cache path is ever
       built out of it. */
    registry_artifact_list list;
    EXPECT_FALSE(registry_fetch_releases(registry_kind_toolchain, "../../etc/passwd",
                                         NULL, NULL, false, &list));

    home_teardown(&fixture);
}
