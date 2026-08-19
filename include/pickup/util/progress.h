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
    int percent;   /* 0 to 100, clamped */
    size_t filled; /* cells to draw as done */
    size_t width;  /* cells in total */
} progress_bar;

/* Work out the bar for `done` of `total` bytes.

   A total of zero or less means the size is unknown, which yields 0%: claiming
   completion for something never measured would be a lie the caller then
   prints. More done than total is clamped, since a file can be longer than
   announced but the bar cannot. */
[[nodiscard]] progress_bar progress_measure(long long done, long long total, size_t width);

/* Draw a labelled bar over the current line. */
void progress_draw(FILE *out, const char *label, long long done, long long total);

/* The same, for work counted in steps rather than in bytes.

   Probing compilers is not a transfer: what advances is how many of them have
   answered, and a byte count would be a figure with nothing behind it. */
void progress_draw_steps(FILE *out, const char *label, long long done, long long total);

/* Finish the line a bar was drawn on. */
void progress_done(FILE *out);

/* Wipe the line a bar or spinner was drawn on, leaving the cursor at its start.

   For work whose result is printed afterwards: the table, or the TOML, is what
   the user asked for, and it should read exactly as if nothing had been drawn
   in front of it. */
void progress_clear(FILE *out);

/* The line a command draws its bar on, and whether it ever did.

   Whether anything appears at all depends on the terminal and on whether there
   was slow work to report — a cached inventory has none. Wiping a line that was
   never drawn would paint a hundred spaces over whatever the terminal had
   there, so the two cases are told apart rather than guessed at. */
typedef struct {
    bool drawn;
} progress_line;

/* Wipe the line, if anything was ever drawn on it. */
void progress_line_clear(FILE *out, progress_line *line);

/* True if `out` is a terminal, and so worth drawing a bar on at all. Piped to
   a file or a log, a bar would only leave a trail of carriage returns. */
[[nodiscard]] bool progress_is_interactive(FILE *out);

/* Frames the spinner cycles through. Four, so it reads as rotation. */
#define SPINNER_FRAMES 4

/* Draw one frame of an indeterminate spinner, with the bytes produced so far.

   For work whose total is not knowable in advance, which is what unpacking is:
   how much comes out of an archive is not written anywhere on it, and reading
   it to find out costs a second pass over the whole thing. Turning minutes of
   silence into something moving is what this is for.

   `frame` may grow without bound; it wraps. */
void spinner_draw(FILE *out, const char *label, size_t frame, long long done);

/* The same, for work that has produced nothing to report yet. Turning up a
   byte count of zero over and over reads as a stall; saying what is going on
   does not. */
void spinner_wait(FILE *out, const char *label, size_t frame);

#endif /* PICKUP_PROGRESS_H */
