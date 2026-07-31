#ifndef PICKUP_SCAN_COMMAND_H
#define PICKUP_SCAN_COMMAND_H

/* Execute `pickup scan`: probe every compiler again and rewrite the cache.
   Returns a pickup_exit_code. */
[[nodiscard]] int scan_command_run(void);

#endif /* PICKUP_SCAN_COMMAND_H */
