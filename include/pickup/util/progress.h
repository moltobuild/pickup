#ifndef PICKUP_PROGRESS_H
#define PICKUP_PROGRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * A progress bar for downloads.
 *
 * The total is known before the transfer starts, because the source reports
 * the size of what it is about to hand over. So progress is measured from
 * bytes on disk rather than from anything the transfer has to volunteer.
 *
 * The measuring is kept apart from the drawing: what a bar looks like at a
 * given fraction is decided by a function that returns numbers, which is what
 * the tests check, while writing it to a terminal is a separate concern.
 */

/* Cells in the bar itself, brackets excluded. */
#define PROGRESS_BAR_WIDTH 25

typedef struct {
    int percent;     /* 0 to 100, clamped */
    size_t filled;   /* cells to draw as done */
    size_t width;    /* cells in total */
} progress_bar;

/* Work out the bar for `done` of `total` bytes.

   A total of zero or less means the size is unknown, which yields 0%: claiming
   completion for something never measured would be a lie the caller then
   prints. More done than total is clamped, since a file can be longer than
   announced but the bar cannot. */
[[nodiscard]] progress_bar progress_measure(long long done, long long total, size_t width);

/* Draw a labelled bar over the current line. */
void progress_draw(FILE *out, const char *label, long long done, long long total);

/* Finish the line a bar was drawn on. */
void progress_done(FILE *out);

/* True if `out` is a terminal, and so worth drawing a bar on at all. Piped to
   a file or a log, a bar would only leave a trail of carriage returns. */
[[nodiscard]] bool progress_is_interactive(FILE *out);

#endif /* PICKUP_PROGRESS_H */
