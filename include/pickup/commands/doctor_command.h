#ifndef PICKUP_DOCTOR_COMMAND_H
#define PICKUP_DOCTOR_COMMAND_H

#include <stdbool.h>

/* Execute `pickup doctor`: report what stops this machine from building, and
   what would fix it.

   Returns exit_ok when nothing is broken, exit_failure when something is. The
   distinction is what lets a script act on the answer; a warning on its own
   does not make the exit code non-zero, because a machine that can still build
   has not failed. */
[[nodiscard]] int doctor_command_run(bool as_toml);

#endif /* PICKUP_DOCTOR_COMMAND_H */
