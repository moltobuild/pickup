#ifndef PICKUP_DOCTOR_COMMAND_H
#define PICKUP_DOCTOR_COMMAND_H

#include <stdbool.h>

/* Execute `pickup doctor`: report what stops this machine from building, and
   what would fix it.

   Returns exit_ok when nothing is broken, exit_failure when something is. The
   distinction is what lets a script act on the answer; a warning on its own
   does not make the exit code non-zero, because a machine that can still build
   has not failed. */
/* `all` includes what is true but not urgent: things that are broken while
   something else covers the need. The machine form always carries everything
   and marks each one, so this only shapes what a person reads. */
[[nodiscard]] int doctor_command_run(bool as_toml, bool all);

#endif /* PICKUP_DOCTOR_COMMAND_H */
