#ifndef PICKUP_PROCESS_SERVICE_H
#define PICKUP_PROCESS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Result of running a command that Pickup interrogates rather than displays. */
typedef struct {
    int exit_code;   /* the child's exit status, or -1 if it never ran */
    bool completed;  /* false if the process could not be started or waited on */
} process_result;

/* A child still running. */
typedef struct {
    pid_t pid;
    bool running;
} process_handle;

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

/* Start `argv` and return without waiting for it, with its output discarded.

   Used where something has to happen while the child runs: a download reports
   its progress from the file growing on disk, which cannot be observed by a
   caller already blocked in wait. Every started handle must end up in
   process_wait, or the child is left a zombie. */
[[nodiscard]] bool process_start(const char *const argv[], process_handle *out);

/* Ask whether a started child has finished, without blocking. Returns true
   once it has, filling `out` with its result; false while it is still
   running. */
[[nodiscard]] bool process_poll(process_handle *handle, process_result *out);

/* Wait for a started child to finish and report how it went. */
process_result process_wait(process_handle *handle);

#endif /* PICKUP_PROCESS_SERVICE_H */
