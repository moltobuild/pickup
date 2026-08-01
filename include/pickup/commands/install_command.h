#ifndef PICKUP_INSTALL_COMMAND_H
#define PICKUP_INSTALL_COMMAND_H

#include <stdbool.h>

typedef struct {
    const char *name;        /* the toolchain to install */
    const char *version;     /* exact or partial; NULL for the latest stable */
    bool allow_unverified;   /* install even when no digest is published */
    bool dry_run;            /* resolve and report, download nothing */
} install_command_request;

/* Install a toolchain under the pickup home. Returns an exit code. */
[[nodiscard]] int install_command_run(const install_command_request *request);

#endif /* PICKUP_INSTALL_COMMAND_H */
