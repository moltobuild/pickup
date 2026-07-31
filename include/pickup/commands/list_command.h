#ifndef PICKUP_LIST_COMMAND_H
#define PICKUP_LIST_COMMAND_H

#include <stdbool.h>

/* Execute `pickup list`: print every toolchain found on this machine.
   With `as_toml`, emit the machine-readable form. Returns a pickup_exit_code. */
[[nodiscard]] int list_command_run(bool as_toml);

#endif /* PICKUP_LIST_COMMAND_H */
