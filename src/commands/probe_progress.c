#include <pickup/commands/probe_progress.h>

#include <pickup/util/progress.h>

#include <stdio.h>

/* What the bar says while the compilers are being asked what they can do. */
#define PROBE_LABEL "probing compilers"

static void watch_probe(size_t done, size_t total, void *context) {
    progress_line *line = context;
    if (!progress_is_interactive(stderr))
        return;
    progress_draw_steps(stderr, PROBE_LABEL, (long long)done, (long long)total);
    line->drawn = true;
}

bool probe_progress_load(inventory *out, bool refresh) {
    progress_line line = {.drawn = false};
    bool loaded = inventory_load_watched(out, refresh, watch_probe, &line);
    /* Wiped whether the load worked or not: a failure prints a message of its
       own, and half a bar in front of it reads as part of the message. */
    progress_line_clear(stderr, &line);
    return loaded;
}
