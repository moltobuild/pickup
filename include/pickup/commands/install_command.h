#ifndef PICKUP_INSTALL_COMMAND_H
#define PICKUP_INSTALL_COMMAND_H

#include <stdbool.h>

typedef struct {
    const char *name;     /* what the registry publishes it as */
    const char *version;  /* exact or partial; NULL for the newest offered */
    bool dry_run;         /* resolve and report, download nothing */
    bool refresh;         /* ignore the cached catalogue and ask again */
} install_command_request;

/* Install a toolchain or a tool under the pickup home. Returns an exit code. */
[[nodiscard]] int install_command_run(const install_command_request *request);

#endif /* PICKUP_INSTALL_COMMAND_H */
