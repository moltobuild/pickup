#include <moltest.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A scratch directory the tests own. */
typedef struct {
    char root[64];
} http_fixture;

static bool fixture_setup(http_fixture *fixture) {
    snprintf(fixture->root, sizeof fixture->root, "%s", "/tmp/pickup_http_XXXXXX");
    return mkdtemp(fixture->root) != NULL;
}

static void fixture_teardown(http_fixture *fixture) {
    (void)fs_remove_tree(fixture->root);
}

/* The transport is exercised over file:// so the tests stay deterministic and
   offline: what is under test is Pickup's handling of curl, not the network. */
static void file_url(const char *path, char *out, size_t out_size) {
    snprintf(out, out_size, "file://%s", path);
}

MOLTEST(http_reports_whether_it_can_download_at_all) {
    /* Whatever the answer, it must be the same every time it is asked. */
    bool first = http_available();
    EXPECT_TRUE(http_available() == first);
    EXPECT_STREQ("curl", http_requirement());
}

MOLTEST(http_downloads_to_the_destination) {
    if (!http_available())
        SKIP("curl is not installed");

    http_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char source[128], dest[128], url[256];
    snprintf(source, sizeof source, "%s/source.txt", fixture.root);
    snprintf(dest, sizeof dest, "%s/dest.txt", fixture.root);
    ASSERT_TRUE(fs_write_file(source, "toolchain payload"));
    file_url(source, url, sizeof url);

    EXPECT_TRUE(http_download(url, dest));

    char *got = fs_read_file(dest);
    ASSERT_NOT_NULL(got);
    EXPECT_STREQ("toolchain payload", got);
    free(got);

    fixture_teardown(&fixture);
}

MOLTEST(http_leaves_nothing_behind_when_the_source_is_missing) {
    if (!http_available())
        SKIP("curl is not installed");

    http_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char dest[128], url[256];
    snprintf(dest, sizeof dest, "%s/dest.bin", fixture.root);
    char missing[128];
    snprintf(missing, sizeof missing, "%s/not-there.bin", fixture.root);
    file_url(missing, url, sizeof url);

    EXPECT_FALSE(http_download(url, dest));

    /* An empty or partial file under the archive's name would be verified
       later and reported as a corrupt download instead of a failed one. */
    EXPECT_FALSE(fs_path_exists(dest));

    fixture_teardown(&fixture);
}

MOLTEST(http_with_progress_falls_back_when_there_is_no_terminal) {
    if (!http_available())
        SKIP("curl is not installed");

    http_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char source[128], dest[128], url[256];
    snprintf(source, sizeof source, "%s/big.bin", fixture.root);
    snprintf(dest, sizeof dest, "%s/copy.bin", fixture.root);
    ASSERT_TRUE(fs_write_file(source, "0123456789"));
    file_url(source, url, sizeof url);

    /* Under a test runner stdout is not a terminal, so this must still deliver
       the file, just without drawing anything. */
    EXPECT_TRUE(http_download_with_progress(url, dest, 10, "downloading"));

    long long size = 0;
    EXPECT_TRUE(fs_file_size(dest, &size));
    EXPECT_TRUE(size == 10);

    fixture_teardown(&fixture);
}

MOLTEST(http_with_progress_reports_a_failure_too) {
    if (!http_available())
        SKIP("curl is not installed");

    http_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char dest[128], url[256], missing[128];
    snprintf(dest, sizeof dest, "%s/copy.bin", fixture.root);
    snprintf(missing, sizeof missing, "%s/absent.bin", fixture.root);
    file_url(missing, url, sizeof url);

    EXPECT_FALSE(http_download_with_progress(url, dest, 1024, "downloading"));
    EXPECT_FALSE(fs_path_exists(dest));

    fixture_teardown(&fixture);
}

/* What a watched download told its caller. */
typedef struct {
    size_t ticks;
    size_t last_frame;
    bool frames_grow;
} tick_record;

static void record_tick(size_t frame, void *context) {
    tick_record *record = context;
    if (record->ticks > 0 && frame <= record->last_frame)
        record->frames_grow = false;
    record->last_frame = frame;
    record->ticks++;
}

MOLTEST(http_watched_delivers_the_file_it_was_asked_for) {
    if (!http_available())
        SKIP("curl is not installed");

    http_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char source[128], dest[128], url[256];
    snprintf(source, sizeof source, "%s/index.json", fixture.root);
    snprintf(dest, sizeof dest, "%s/copy.json", fixture.root);
    ASSERT_TRUE(fs_write_file(source, "[]"));
    file_url(source, url, sizeof url);

    /* A local copy may well finish before the first poll, so the tick count is
       not what is under test here: the transfer is. Watching must not cost the
       caller the file. */
    tick_record record = { .ticks = 0, .last_frame = 0, .frames_grow = true };
    EXPECT_TRUE(http_download_watched(url, dest, record_tick, &record));

    char *got = fs_read_file(dest);
    ASSERT_NOT_NULL(got);
    EXPECT_STREQ("[]", got);
    free(got);
    EXPECT_TRUE(record.frames_grow); /* a frame that went backwards would stall the spinner */

    fixture_teardown(&fixture);
}

MOLTEST(http_watched_without_a_watcher_is_a_plain_download) {
    if (!http_available())
        SKIP("curl is not installed");

    http_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char source[128], dest[128], url[256];
    snprintf(source, sizeof source, "%s/index.json", fixture.root);
    snprintf(dest, sizeof dest, "%s/copy.json", fixture.root);
    ASSERT_TRUE(fs_write_file(source, "[]"));
    file_url(source, url, sizeof url);

    EXPECT_TRUE(http_download_watched(url, dest, NULL, NULL));
    EXPECT_TRUE(fs_path_exists(dest));

    fixture_teardown(&fixture);
}

MOLTEST(http_watched_leaves_nothing_behind_when_it_fails) {
    if (!http_available())
        SKIP("curl is not installed");

    http_fixture fixture;
    ASSERT_TRUE(fixture_setup(&fixture));

    char dest[128], url[256], missing[128];
    snprintf(dest, sizeof dest, "%s/copy.json", fixture.root);
    snprintf(missing, sizeof missing, "%s/absent.json", fixture.root);
    file_url(missing, url, sizeof url);

    tick_record record = { .ticks = 0, .last_frame = 0, .frames_grow = true };
    EXPECT_FALSE(http_download_watched(url, dest, record_tick, &record));
    EXPECT_FALSE(fs_path_exists(dest));

    fixture_teardown(&fixture);
}
