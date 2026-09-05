#ifndef PICKUP_PROCESS_SERVICE_H
#define PICKUP_PROCESS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Result of running a command that Pickup interrogates rather than displays. */
typedef struct {
    int exit_code;  /* the child's exit status, or -1 if it never ran */
    bool completed; /* false if the process could not be started or waited on */
} process_result;

/* A child still running.
 *
 * What identifies the child is the one thing in this header that a platform
 * decides: a pid on POSIX, an object handle on Windows, and the two are not
 * the same size or the same kind of thing. No caller reads the field — they
 * pass the handle back to `process_poll` and `process_wait` — so the `#ifdef`
 * stays here and never reaches the code that spawns anything (RFC-0017). */
typedef struct {
#ifdef _WIN32
    void *process; /* HANDLE, opaque so callers need no windows.h */
#else
    pid_t pid;
#endif
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
[[nodiscard]] process_result process_capture(const char *const argv[], const char *input, char *out,
                                             size_t out_size);

/* The same, capturing stderr instead.

   Compilers say two different kinds of thing on two different streams. What a
   compiler *is* comes back on stdout, which is what process_capture reads; what
   it *decided* — which GCC installation it picked, which directories it
   searched — is written to stderr by `-v`. Reading that is the only way to
   learn a choice the driver makes internally and never prints anywhere else. */
[[nodiscard]] process_result process_capture_stderr(const char *const argv[], const char *input,
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

/* End a started child that is not going to finish on its own.
 *
 * For a watchdog, and only for one: a caller that can tell a working child from
 * a wedged one, and has decided. The handle is not settled by this call --
 * `process_poll` or `process_wait` still has to observe the child and collect
 * it, exactly as if it had ended by itself. */
void process_stop(process_handle *handle);

/* Pause for `milliseconds`, between one `process_poll` and the next.
 *
 * It belongs beside poll because it exists for poll: a caller watching a child
 * has to wait before asking again. The two places that do were each spelling
 * it with `nanosleep`, which mingw declares and does not provide — a call that
 * compiles cleanly on Windows and then fails at link, which is why syntax
 * checking alone never found it. */
void process_pause_ms(unsigned milliseconds);

#endif /* PICKUP_PROCESS_SERVICE_H */
