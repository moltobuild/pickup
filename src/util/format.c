#include <pickup/util/format.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Step between one unit and the next. */
#define UNIT_STEP 1024.0

/* Below this a decimal still says something; at or above it, it does not. */
#define DECIMAL_LIMIT 10.0

static const char *const unit_names[] = { "B", "K", "M", "G", "T" };
#define UNIT_COUNT (sizeof unit_names / sizeof unit_names[0])

void format_size(long long bytes, char *out, size_t out_size) {
    if (bytes < 0) {
        snprintf(out, out_size, "-");
        return;
    }

    double value = (double)bytes;
    size_t unit = 0;
    while (value >= UNIT_STEP && unit + 1 < UNIT_COUNT) {
        value /= UNIT_STEP;
        unit++;
    }

    /* Whole bytes never get a decimal: "512B", not "512.0B". */
    if (unit == 0)
        snprintf(out, out_size, "%lld%s", bytes, unit_names[unit]);
    else if (value < DECIMAL_LIMIT)
        snprintf(out, out_size, "%.1f%s", value, unit_names[unit]);
    else
        snprintf(out, out_size, "%.0f%s", value, unit_names[unit]);
}

/* The marks, and what stands in for them elsewhere. */
#define CHECK_UTF8 "\xe2\x9c\x93" /* U+2713 CHECK MARK */
#define CROSS_UTF8 "\xe2\x9c\x97" /* U+2717 BALLOT X */
#define CHECK_ASCII "ok"
#define CROSS_ASCII "x"

/* The locale settings that say how output will be read, most specific first. */
static const char *const locale_variables[] = { "LC_ALL", "LC_CTYPE", "LANG" };
#define LOCALE_COUNT (sizeof locale_variables / sizeof locale_variables[0])

static bool console_speaks_utf8(void) {
    for (size_t i = 0; i < LOCALE_COUNT; i++) {
        const char *value = getenv(locale_variables[i]);
        if (value == NULL || value[0] == '\0')
            continue;
        /* The first one set decides, the way the C library resolves them. */
        return strstr(value, "UTF-8") != NULL || strstr(value, "utf8") != NULL
            || strstr(value, "UTF8") != NULL || strstr(value, "utf-8") != NULL;
    }
    return false;
}

const char *format_check(void) {
    return console_speaks_utf8() ? CHECK_UTF8 : CHECK_ASCII;
}

const char *format_cross(void) {
    return console_speaks_utf8() ? CROSS_UTF8 : CROSS_ASCII;
}
