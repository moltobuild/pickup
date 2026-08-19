#include <pickup/util/color.h>

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
    if (out == NULL || isatty(fileno(out)) != 1)
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
