#ifndef PICKUP_SEARCH_COMMAND_H
#define PICKUP_SEARCH_COMMAND_H

#include <stdbool.h>

typedef struct {
    const char *name;      /* the toolchain to search for */
    const char *version;   /* exact or partial; NULL for everything on offer */
    bool refresh;          /* ignore the cached index and ask the source again */
    bool as_toml;
} search_command_request;

/* List the versions of a toolchain that can be installed on this machine.
   Release candidates are never listed. Returns an exit code. */
[[nodiscard]] int search_command_run(const search_command_request *request);

#endif /* PICKUP_SEARCH_COMMAND_H */
