#ifndef PICKUP_SEARCH_COMMAND_H
#define PICKUP_SEARCH_COMMAND_H

#include <stdbool.h>

/* List the versions of `name` that can be installed on this machine.

   `version` narrows the answer and may be partial ("14", "14.2", "14.2.1");
   NULL or "" lists everything on offer. Release candidates are never listed.
   Returns an exit code. */
[[nodiscard]] int search_command_run(const char *name, const char *version, bool as_toml);

#endif /* PICKUP_SEARCH_COMMAND_H */
