#include <pickup/util/progress.h>

#include <pickup/util/color.h>
#include <pickup/util/format.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What the bar is drawn with. */
#define CELL_FILLED "\xe2\x96\x88" /* U+2588 FULL BLOCK */
#define CELL_EMPTY  "\xe2\x96\x91" /* U+2591 LIGHT SHADE */

#define PERCENT_MAX 100

progress_bar progress_measure(long long done, long long total, size_t width) {
    progress_bar bar = { .percent = 0, .filled = 0, .width = width };
    if (total <= 0 || done <= 0)
        return bar;

    if (done > total)
        done = total;

    bar.percent = (int)((done * PERCENT_MAX) / total);
    bar.filled = (size_t)(((unsigned long long)done * width) / (unsigned long long)total);
    return bar;
}

void progress_draw(FILE *out, const char *label, long long done, long long total) {
    progress_bar bar = progress_measure(done, total, PROGRESS_BAR_WIDTH);

    char done_text[FORMAT_SIZE_MAX];
    char total_text[FORMAT_SIZE_MAX];
    format_size(done, done_text, sizeof done_text);
    format_size(total, total_text, sizeof total_text);

    fprintf(out, "\r%s  %s[", label, color_accent());
    for (size_t i = 0; i < bar.width; i++)
        fputs(i < bar.filled ? CELL_FILLED : CELL_EMPTY, out);
    fprintf(out, "]%s  %3d%%  %s%s/%s%s", color_reset(), bar.percent,
            color_dim(), done_text, total_text, color_reset());
    fflush(out);
}

/* Read as rotation rather than as four unrelated characters. */
static const char *const spinner_frames[SPINNER_FRAMES] = { "-", "\\", "|", "/" };

void spinner_wait(FILE *out, const char *label, size_t frame) {
    fprintf(out, "\r%s  %s%s%s   ", label,
            color_accent(), spinner_frames[frame % SPINNER_FRAMES], color_reset());
    fflush(out);
}

void spinner_draw(FILE *out, const char *label, size_t frame, long long done) {
    char done_text[FORMAT_SIZE_MAX];
    format_size(done, done_text, sizeof done_text);

    fprintf(out, "\r%s  %s%s%s  %s%s extracted%s", label,
            color_accent(), spinner_frames[frame % SPINNER_FRAMES], color_reset(),
            color_dim(), done_text, color_reset());
    fflush(out);
}

void progress_done(FILE *out) {
    fputc('\n', out);
    fflush(out);
}

bool progress_is_interactive(FILE *out) {
    return isatty(fileno(out)) == 1;
}
