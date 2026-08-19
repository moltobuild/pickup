#ifndef PICKUP_SEARCH_COMMAND_H
#define PICKUP_SEARCH_COMMAND_H

#include <stdbool.h>

typedef struct {
    const char *name;    /* what to look up; NULL lists everything published */
    const char *version; /* exact or partial; NULL for everything on offer */
    bool refresh;        /* ignore the cached catalogue and ask again */
    bool as_toml;
} search_command_request;

/* List what the registry publishes, or the versions of one of it that can be
   installed on this machine. Returns an exit code. */
[[nodiscard]] int search_command_run(const search_command_request *request);

#endif /* PICKUP_SEARCH_COMMAND_H */
