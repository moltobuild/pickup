#include <pickup/util/color.h>

#include <pickup/util/console.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* SGR sequences. Kept to the eight basic colours, which every terminal worth
   colouring for renders, and none of which assume a light or dark background. */
#define SGR_GREEN "\x1b[32m"
#define SGR_RED "\x1b[31m"
#define SGR_CYAN "\x1b[36m"
#define SGR_DIM "\x1b[2m"
#define SGR_RESET "\x1b[0m"

#define NO_COLOUR ""

static bool enabled = false;

bool color_enabled(FILE *out) {
    /* Asked as `== 0` rather than `!= 1`, because 1 is not the answer a
       terminal gives everywhere. POSIX says isatty returns non-zero for a
       terminal and names no particular non-zero; the Windows CRT returns the
       bit that marks a character device, 0x40. A check spelled `== 1` reads a
       Windows console as a pipe, and every bar and spinner behind it silently
       stops being drawn. */
    if (out == NULL || isatty(fileno(out)) == 0)
        return false;

    /* Written only where it will be acted on. A console that has not been put
       into virtual-terminal mode prints the sequence as its own characters,
       which is worse than plain output rather than better. */
    if (!console_supports_sequences())
        return false;

    /* Set to anything at all, NO_COLOR means no colour. */
    const char *no_color = getenv(NO_COLOR_ENV);
    if (no_color != NULL && no_color[0] != '\0')
        return false;

    const char *term = getenv(TERM_ENV);
    if (term != NULL && strcmp(term, TERM_DUMB) == 0)
        return false;
    return true;
}

void color_configure(FILE *out) { enabled = color_enabled(out); }

const char *color_ok(void) { return enabled ? SGR_GREEN : NO_COLOUR; }

const char *color_error(void) { return enabled ? SGR_RED : NO_COLOUR; }

const char *color_accent(void) { return enabled ? SGR_CYAN : NO_COLOUR; }

const char *color_dim(void) { return enabled ? SGR_DIM : NO_COLOUR; }

const char *color_reset(void) { return enabled ? SGR_RESET : NO_COLOUR; }
