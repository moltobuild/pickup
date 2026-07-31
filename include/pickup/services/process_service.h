#ifndef PICKUP_PROCESS_SERVICE_H
#define PICKUP_PROCESS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/* Result of running a command that Pickup interrogates rather than displays. */
typedef struct {
    int exit_code;   /* the child's exit status, or -1 if it never ran */
    bool completed;  /* false if the process could not be started or waited on */
} process_result;

/* Run `argv` with its output discarded, and report only whether it succeeded.
   Used by capability probes, where the answer is the exit status: the compiler
   either accepted the program or it did not. `input` is fed to the child's
   stdin (may be NULL); stderr is silenced so a failing probe stays quiet. */
[[nodiscard]] process_result process_try(const char *const argv[], const char *input);

/* Run `argv` and capture what it writes to stdout into `out`, NUL-terminated
   and truncated to `out_size`. `input` is fed to the child's stdin (may be
   NULL). Used to ask a compiler about itself. */
[[nodiscard]] process_result process_capture(const char *const argv[], const char *input,
                                             char *out, size_t out_size);

#endif /* PICKUP_PROCESS_SERVICE_H */
