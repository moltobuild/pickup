#include <moltest.h>

#include <pickup/util/format.h>
#include <pickup/util/progress.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

MOLTEST(format_size_reads_at_a_glance) {
    char text[FORMAT_SIZE_MAX];

    format_size(0, text, sizeof text);
    EXPECT_STREQ("0B", text);
    format_size(512, text, sizeof text);
    EXPECT_STREQ("512B", text);

    /* A decimal below ten, none above it. */
    format_size(1024, text, sizeof text);
    EXPECT_STREQ("1.0K", text);
    format_size(1024 * 100, text, sizeof text);
    EXPECT_STREQ("100K", text);

    /* The size of a real LLVM archive. */
    format_size(1938859476LL, text, sizeof text);
    EXPECT_STREQ("1.8G", text);
    format_size(1044930068LL, text, sizeof text);
    EXPECT_STREQ("997M", text);
}

MOLTEST(progress_measures_the_obvious_fractions) {
    progress_bar bar = progress_measure(50, 100, 10);
    EXPECT_EQ(50, bar.percent);
    EXPECT_EQ(5, (int)bar.filled);

    bar = progress_measure(100, 100, 10);
    EXPECT_EQ(100, bar.percent);
    EXPECT_EQ(10, (int)bar.filled);

    bar = progress_measure(0, 100, 10);
    EXPECT_EQ(0, bar.percent);
    EXPECT_EQ(0, (int)bar.filled);
}

MOLTEST(progress_survives_a_total_it_was_never_told) {
    /* Nothing is known, so nothing is claimed. */
    progress_bar bar = progress_measure(500, 0, 10);
    EXPECT_EQ(0, bar.percent);
    EXPECT_EQ(0, (int)bar.filled);

    bar = progress_measure(500, -1, 10);
    EXPECT_EQ(0, bar.percent);
}

MOLTEST(progress_clamps_a_file_longer_than_announced) {
    /* A server may deliver more than it promised; the bar still stops at full
       rather than overrunning its own width. */
    progress_bar bar = progress_measure(150, 100, 10);
    EXPECT_EQ(100, bar.percent);
    EXPECT_EQ(10, (int)bar.filled);
}

MOLTEST(progress_does_not_overflow_on_gigabyte_downloads) {
    /* done * 100 and done * width both exceed 32 bits here, which is where a
       narrower intermediate type would wrap and report nonsense. */
    long long total = 1938859476LL;
    progress_bar bar = progress_measure(total / 2, total, PROGRESS_BAR_WIDTH);
    EXPECT_EQ(50, bar.percent);
    EXPECT_EQ(12, (int)bar.filled);

    bar = progress_measure(total, total, PROGRESS_BAR_WIDTH);
    EXPECT_EQ(100, bar.percent);
    EXPECT_EQ(PROGRESS_BAR_WIDTH, (int)bar.filled);
}

MOLTEST(progress_draws_a_bar_with_its_percentage) {
    FILE *sink = tmpfile();
    ASSERT_TRUE(sink != NULL);

    progress_draw(sink, "downloading", 1024 * 1024, 4 * 1024 * 1024);
    rewind(sink);

    char line[256] = "";
    size_t read = fread(line, 1, sizeof line - 1, sink);
    line[read] = '\0';
    fclose(sink);

    /* Redrawn in place, labelled, and carrying both the percentage and the
       sizes it stands for. */
    EXPECT_TRUE(line[0] == '\r');
    EXPECT_TRUE(strstr(line, "downloading") != NULL);
    EXPECT_TRUE(strstr(line, " 25%") != NULL);
    EXPECT_TRUE(strstr(line, "1.0M/4.0M") != NULL);
}

MOLTEST(spinner_cycles_through_its_frames) {
    char seen[SPINNER_FRAMES][64];

    for (size_t frame = 0; frame < SPINNER_FRAMES; frame++) {
        FILE *sink = tmpfile();
        ASSERT_TRUE(sink != NULL);
        spinner_draw(sink, "extracting", frame, 1024);
        rewind(sink);
        size_t read = fread(seen[frame], 1, sizeof seen[frame] - 1, sink);
        seen[frame][read] = '\0';
        fclose(sink);
    }

    /* Every frame differs from the one before, which is what reads as motion
       rather than as a stuck character. */
    for (size_t frame = 1; frame < SPINNER_FRAMES; frame++)
        EXPECT_TRUE(strcmp(seen[frame], seen[frame - 1]) != 0);

    /* And it wraps rather than walking off the end of the set. */
    FILE *sink = tmpfile();
    ASSERT_TRUE(sink != NULL);
    spinner_draw(sink, "extracting", SPINNER_FRAMES * 1000, 1024);
    rewind(sink);
    char wrapped[64] = "";
    size_t read = fread(wrapped, 1, sizeof wrapped - 1, sink);
    wrapped[read] = '\0';
    fclose(sink);
    EXPECT_STREQ(seen[0], wrapped);
}

MOLTEST(spinner_waits_without_claiming_a_figure) {
    FILE *sink = tmpfile();
    ASSERT_TRUE(sink != NULL);
    spinner_wait(sink, "preparing the extraction", 1);
    rewind(sink);

    char line[128] = "";
    size_t read = fread(line, 1, sizeof line - 1, sink);
    line[read] = '\0';
    fclose(sink);

    /* Before anything has been produced there is no number worth showing, and
       a byte count stuck at zero reads as a stall rather than as work. */
    EXPECT_TRUE(line[0] == '\r');
    EXPECT_TRUE(strstr(line, "preparing the extraction") != NULL);
    EXPECT_TRUE(strstr(line, "0B") == NULL);
    EXPECT_TRUE(strstr(line, "extracted") == NULL);
}

MOLTEST(spinner_says_how_much_it_has_produced) {
    FILE *sink = tmpfile();
    ASSERT_TRUE(sink != NULL);
    spinner_draw(sink, "extracting clang 22.1.8", 0, 224395264);
    rewind(sink);

    char line[128] = "";
    size_t read = fread(line, 1, sizeof line - 1, sink);
    line[read] = '\0';
    fclose(sink);

    /* Without a percentage to give, the bytes so far are what tells the user
       it is moving. */
    EXPECT_TRUE(line[0] == '\r');
    EXPECT_TRUE(strstr(line, "extracting clang 22.1.8") != NULL);
    EXPECT_TRUE(strstr(line, "214M") != NULL);
}

MOLTEST(progress_is_not_interactive_when_redirected) {
    FILE *sink = tmpfile();
    ASSERT_TRUE(sink != NULL);
    EXPECT_FALSE(progress_is_interactive(sink));
    fclose(sink);
}
