#include <pickup/services/http_service.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/process_service.h>
#include <pickup/util/progress.h>

#include <stdio.h>
#include <time.h>

/* The downloader Pickup drives, and the flags it is driven with.
   -f  treat an HTTP error as a failure instead of saving the error page
   -sS quiet, but still say why it failed
   -L  follow redirects, which is how release downloads are served */
#define CURL_COMMAND  "curl"
#define ARG_FAIL      "-f"
#define ARG_SILENT    "-sS"
#define ARG_LOCATION  "-L"
#define ARG_OUTPUT    "-o"
#define ARG_VERSION   "--version"

/* How often the growing file is measured while drawing the bar. */
#define POLL_INTERVAL_NS 100000000L /* 0.1 s */

const char *http_requirement(void) {
    return CURL_COMMAND;
}

bool http_available(void) {
    static bool checked = false;
    static bool available = false;
    if (checked)
        return available;

    const char *argv[] = { CURL_COMMAND, ARG_VERSION, NULL };
    process_result result = process_try(argv, NULL);
    available = result.completed && result.exit_code == 0;
    checked = true;
    return available;
}

/* Nothing half-written is ever left behind under the destination name. */
static bool discard(const char *path) {
    remove(path);
    return false;
}

/* Number of entries build_argv fills, terminator included. */
#define CURL_ARGV_SIZE 8

static void build_argv(const char *url, const char *dest, const char *argv[CURL_ARGV_SIZE]) {
    argv[0] = CURL_COMMAND;
    argv[1] = ARG_FAIL;
    argv[2] = ARG_SILENT;
    argv[3] = ARG_LOCATION;
    argv[4] = ARG_OUTPUT;
    argv[5] = dest;
    argv[6] = url;
    argv[7] = NULL;
}

bool http_download(const char *url, const char *dest) {
    if (!http_available())
        return false;

    const char *argv[CURL_ARGV_SIZE];
    build_argv(url, dest, argv);

    process_result result = process_try(argv, NULL);
    if (!result.completed || result.exit_code != 0)
        return discard(dest);
    return true;
}

static void wait_a_moment(void) {
    struct timespec pause = { .tv_sec = 0, .tv_nsec = POLL_INTERVAL_NS };
    nanosleep(&pause, NULL);
}

/* Draw the bar from the size of the file as it grows, which is why the total
   has to be known in advance: curl is not asked anything. */
static void draw_current(FILE *out, const char *label, const char *dest,
                         long long expected_size) {
    long long done = 0;
    if (!fs_file_size(dest, &done))
        done = 0;
    progress_draw(out, label, done, expected_size);
}

bool http_download_with_progress(const char *url, const char *dest,
                                 long long expected_size, const char *label) {
    if (!http_available())
        return false;

    bool show_bar = expected_size > 0 && progress_is_interactive(stdout);
    if (!show_bar)
        return http_download(url, dest);

    const char *argv[CURL_ARGV_SIZE];
    build_argv(url, dest, argv);

    process_handle handle;
    if (!process_start(argv, &handle))
        return discard(dest);

    process_result result;
    while (!process_poll(&handle, &result)) {
        draw_current(stdout, label, dest, expected_size);
        wait_a_moment();
    }

    if (!result.completed || result.exit_code != 0) {
        progress_done(stdout);
        return discard(dest);
    }

    /* Land on a full bar rather than wherever the last poll happened to catch
       it, so a finished download does not read as 98%. */
    progress_draw(stdout, label, expected_size, expected_size);
    progress_done(stdout);
    return true;
}
