#include <moltest.h>

#include <pickup/services/process_service.h>

#include <stdlib.h>
#include <string.h>

MOLTEST(process_try_reports_the_exit_status) {
    const char *const ok[] = { "sh", "-c", "exit 0", NULL };
    process_result result = process_try(ok, NULL);
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(0, result.exit_code);

    const char *const bad[] = { "sh", "-c", "exit 3", NULL };
    result = process_try(bad, NULL);
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(3, result.exit_code);
}

MOLTEST(process_try_feeds_stdin) {
    /* A capability probe is exactly this: a program handed over on stdin. */
    const char *const argv[] = { "sh", "-c", "read line; test \"$line\" = probe", NULL };
    process_result result = process_try(argv, "probe\n");
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(0, result.exit_code);
}

MOLTEST(process_try_reports_a_command_that_cannot_run) {
    const char *const argv[] = { "/nonexistent/program", NULL };
    process_result result = process_try(argv, NULL);
    /* The fork succeeded, so this completed: what failed is the command, and
       its status says so. */
    EXPECT_TRUE(result.completed);
    EXPECT_TRUE(result.exit_code != 0);
}

MOLTEST(process_capture_collects_stdout) {
    char out[64] = "";
    const char *const argv[] = { "sh", "-c", "printf hello", NULL };
    process_result result = process_capture(argv, NULL, out, sizeof out);
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(0, result.exit_code);
    EXPECT_STREQ("hello", out);
}

MOLTEST(process_capture_truncates_without_overflowing) {
    char out[8] = "";
    const char *const argv[] = { "sh", "-c", "printf 0123456789abcdef", NULL };
    process_result result = process_capture(argv, NULL, out, sizeof out);
    EXPECT_TRUE(result.completed);
    EXPECT_EQ(7, (int)strlen(out)); /* size - 1, still NUL-terminated */
}

MOLTEST(process_capture_keeps_stderr_out_of_the_answer) {
    /* Probe noise must never reach the caller's parsing. */
    char out[64] = "";
    const char *const argv[] = { "sh", "-c", "printf out; printf err >&2", NULL };
    process_result result = process_capture(argv, NULL, out, sizeof out);
    EXPECT_TRUE(result.completed);
    EXPECT_STREQ("out", out);
}
