#ifndef PICKUP_DEFAULT_COMMAND_H
#define PICKUP_DEFAULT_COMMAND_H

#include <stdbool.h>

/* What `pickup default` was asked to do. */
typedef struct {
    const char *name;   /* the toolchain to prefer; NULL reports the current one */
    bool clear;         /* forget the preference instead of setting one */
    bool as_toml;
} default_command_request;

/* Execute `pickup default`. Returns a pickup_exit_code. */
[[nodiscard]] int default_command_run(const default_command_request *request);

#endif /* PICKUP_DEFAULT_COMMAND_H */
