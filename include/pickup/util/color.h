#ifndef PICKUP_COLOR_H
#define PICKUP_COLOR_H

#include <stdbool.h>
#include <stdio.h>

/*
 * Colour for output people read.
 *
 * Every accessor returns an empty string when colour does not apply, so callers
 * print them unconditionally instead of branching around them. That is what
 * keeps a redirected run free of escape sequences: piped into a file or a log,
 * the output is exactly the plain text it always was.
 *
 * Colour is used when writing to a terminal, unless `NO_COLOR` is set — the
 * convention pip and the rest honour — or the terminal says it is `dumb`.
 */

/* Environment that turns colour off regardless of the terminal. */
#define NO_COLOR_ENV "NO_COLOR"
#define TERM_ENV     "TERM"
#define TERM_DUMB    "dumb"

/* True if `out` should be written to in colour. */
[[nodiscard]] bool color_enabled(FILE *out);

/* Decide once, for the stream the reports go to. Until this is called nothing
   is coloured, so a caller that never opts in is unaffected. */
void color_configure(FILE *out);

/* Success, failure, the thing being emphasised, and detail that should not
   compete with it. Empty strings unless colour is on. */
[[nodiscard]] const char *color_ok(void);
[[nodiscard]] const char *color_error(void);
[[nodiscard]] const char *color_accent(void);
[[nodiscard]] const char *color_dim(void);

/* Back to the terminal's own colours. */
[[nodiscard]] const char *color_reset(void);

#endif /* PICKUP_COLOR_H */
