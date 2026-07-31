#ifndef PICKUP_SHOW_COMMAND_H
#define PICKUP_SHOW_COMMAND_H

#include <stdbool.h>

/* Execute `pickup show <name>`: print one toolchain in detail, feature by
   feature. With `as_toml`, emit the machine-readable form.
   Returns a pickup_exit_code. */
[[nodiscard]] int show_command_run(const char *name, bool as_toml);

#endif /* PICKUP_SHOW_COMMAND_H */
