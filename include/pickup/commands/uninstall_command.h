#ifndef PICKUP_UNINSTALL_COMMAND_H
#define PICKUP_UNINSTALL_COMMAND_H

#include <stdbool.h>

/* Execute `pickup uninstall <name>`: remove a toolchain Pickup installed.

   `assume_yes` skips the confirmation. Without it, a terminal is asked and a
   pipe is not: naming a toolchain on the command line is already the
   intention, and a script that named one cannot answer a prompt.

   Returns a pickup_exit_code. */
[[nodiscard]] int uninstall_command_run(const char *name, bool assume_yes);

#endif /* PICKUP_UNINSTALL_COMMAND_H */
