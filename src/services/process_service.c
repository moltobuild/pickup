#include <pickup/services/process_service.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Exit code a shell reports when a command cannot be executed. */
#define EXIT_COMMAND_NOT_RUNNABLE 127

static process_result failed_result(void) {
    return (process_result){.exit_code = -1, .completed = false};
}

/* Which of the child's streams the caller wants back. The third value is not
   an absence of an answer: it says both streams are silenced, which is what a
   probe wants — a failing probe is an answer, not something to report. */
typedef enum {
    capture_none,
    capture_out,
    capture_err,
} capture_kind;

/*
 * Below this line the file is written twice, once per platform, and above it
 * nothing knows which one it is (RFC-0017). Each half provides the same four
 * things and the public functions at the bottom are shared:
 *
 *   run    — start, feed stdin, capture at most one stream, wait
 *   start  — begin and hand back a handle
 *   poll   — has it finished, without blocking
 *   wait   — block until it has
 */

#ifdef _WIN32

/* --- Windows --- */

/*
 * Windows takes a command line and the child pulls its argument vector back
 * out, so the quoting written here and the unquoting done there have to agree
 * exactly: a backslash is literal unless it runs into a quote, and a run of
 * them before a quote is halved.
 *
 * Getting this wrong does not fail loudly. It hands a compiler a path with one
 * backslash too few, and the error comes back from the compiler about a file
 * that nearly exists.
 */
static bool put(char *out, size_t size, size_t *at, char c) {
    if (*at + 1 >= size)
        return false;
    out[(*at)++] = c;
    return true;
}

static bool quote_arg(char *out, size_t size, size_t *at, const char *arg) {
    const bool bare = arg[0] != '\0' && strpbrk(arg, " \t\n\v\"") == NULL;
    if (bare) {
        for (const char *c = arg; *c != '\0'; c++) {
            if (!put(out, size, at, *c))
                return false;
        }
        return true;
    }

    if (!put(out, size, at, '"'))
        return false;
    for (const char *c = arg;; c++) {
        size_t slashes = 0;
        while (*c == '\\') {
            slashes++;
            c++;
        }
        if (*c == '\0') {
            /* Doubled: they are about to sit before the closing quote, and a
               lone backslash there would escape it. */
            for (size_t i = 0; i < slashes * 2; i++) {
                if (!put(out, size, at, '\\'))
                    return false;
            }
            break;
        }
        const size_t emit = *c == '"' ? slashes * 2 + 1 : slashes;
        for (size_t i = 0; i < emit; i++) {
            if (!put(out, size, at, '\\'))
                return false;
        }
        if (!put(out, size, at, *c))
            return false;
    }
    return put(out, size, at, '"');
}

/* The documented ceiling for a command line. */
#define COMMAND_LINE_MAX 32768

static bool build_command_line(const char *const argv[], char *out, size_t size) {
    size_t at = 0;
    for (size_t i = 0; argv[i] != NULL; i++) {
        if (i > 0 && !put(out, size, &at, ' '))
            return false;
        if (!quote_arg(out, size, &at, argv[i]))
            return false;
    }
    if (at + 1 >= size)
        return false;
    out[at] = '\0';
    return true;
}

/* An inheritable handle onto the null device, for a stream nobody reads. */
static HANDLE open_null(DWORD access) {
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    return CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                       NULL);
}

static void close_handle(HANDLE h) {
    if (h != NULL && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/* A pipe whose parent end the child must not inherit: left inheritable, the
   child would hold the write end open and the parent's read would never see
   end of file. */
static bool make_pipe(HANDLE *read_end, HANDLE *write_end, bool parent_reads) {
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    if (!CreatePipe(read_end, write_end, &sa, 0))
        return false;
    HANDLE parent = parent_reads ? *read_end : *write_end;
    if (!SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0)) {
        close_handle(*read_end);
        close_handle(*write_end);
        return false;
    }
    return true;
}

static process_result reap_handle(HANDLE process) {
    if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
        close_handle(process);
        return failed_result();
    }
    DWORD code = 0;
    const bool got = GetExitCodeProcess(process, &code) != 0;
    close_handle(process);
    if (!got)
        return failed_result();
    return (process_result){.exit_code = (int)code, .completed = true};
}

/* `lpApplicationName` is left NULL so that the search rules apply to the first
   token — which is what makes `gcc` find `gcc.exe` on PATH, the way execvp
   finds `gcc`. */
static bool launch(const char *const argv[], HANDLE in, HANDLE out, HANDLE err,
                   PROCESS_INFORMATION *info) {
    char command[COMMAND_LINE_MAX];
    if (!build_command_line(argv, command, sizeof command))
        return false;

    STARTUPINFOA startup = {.cb = sizeof startup,
                            .dwFlags = STARTF_USESTDHANDLES,
                            .hStdInput = in,
                            .hStdOutput = out,
                            .hStdError = err};
    return CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, info) != 0;
}

/* What POSIX says when a command cannot be started, said on Windows.

   There is no fork/exec split here: `CreateProcess` fails as one act, so the
   parent learns of a missing program directly where POSIX learns of it from a
   child that already existed and exited 127. Reporting that as "did not
   complete" would make the same missing compiler two different answers
   depending on the platform, and every caller would have to know which — which
   is the platform knowledge RFC-0017 keeps inside this file. So the shape POSIX
   produces is the shape this returns: it ran, and it was not runnable.

   Anything else — no memory, no handles, a broken image — really is a failure
   to run the request at all, and stays one. */
static process_result not_runnable_or_failed(DWORD why) {
    if (why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND || why == ERROR_BAD_EXE_FORMAT ||
        why == ERROR_ACCESS_DENIED)
        return (process_result){.exit_code = EXIT_COMMAND_NOT_RUNNABLE, .completed = true};
    return failed_result();
}

static process_result platform_run(const char *const argv[], const char *input, capture_kind what,
                                   char *out, size_t out_size) {
    HANDLE stdin_read = NULL, stdin_write = NULL;
    if (!make_pipe(&stdin_read, &stdin_write, false))
        return failed_result();

    HANDLE capture_read = NULL, capture_write = NULL;
    if (what != capture_none && !make_pipe(&capture_read, &capture_write, true)) {
        close_handle(stdin_read);
        close_handle(stdin_write);
        return failed_result();
    }

    HANDLE null_out = open_null(GENERIC_WRITE);
    HANDLE child_out = what == capture_out ? capture_write : null_out;
    HANDLE child_err = what == capture_err ? capture_write : null_out;

    PROCESS_INFORMATION info = {0};
    const bool started = launch(argv, stdin_read, child_out, child_err, &info);
    /* Read here and not where it is used: every CloseHandle below sets a last
       error of its own, and one of them would answer for the launch. */
    const DWORD why = started ? ERROR_SUCCESS : GetLastError();

    /* The child owns its ends now; holding them here would keep the pipe open
       against ourselves. */
    close_handle(stdin_read);
    close_handle(capture_write);
    close_handle(null_out);

    if (!started) {
        close_handle(stdin_write);
        close_handle(capture_read);
        return not_runnable_or_failed(why);
    }
    close_handle(info.hThread);

    if (input != NULL) {
        DWORD written = 0;
        /* A short write only means the child stopped reading, which its exit
           status already reports. */
        (void)WriteFile(stdin_write, input, (DWORD)strlen(input), &written, NULL);
    }
    close_handle(stdin_write); /* end of file, so the child stops waiting */

    if (what != capture_none) {
        /* Drained before waiting: a child that fills the pipe would block
           forever if we waited first. */
        size_t total = 0;
        while (total + 1 < out_size) {
            DWORD got = 0;
            if (!ReadFile(capture_read, out + total, (DWORD)(out_size - total - 1), &got, NULL) ||
                got == 0)
                break;
            total += got;
        }
        out[total] = '\0';
        close_handle(capture_read);
    }
    return reap_handle(info.hProcess);
}

static bool platform_start(const char *const argv[], process_handle *out) {
    HANDLE null_in = open_null(GENERIC_READ);
    HANDLE null_out = open_null(GENERIC_WRITE);
    PROCESS_INFORMATION info = {0};
    const bool started = launch(argv, null_in, null_out, null_out, &info);
    close_handle(null_in);
    close_handle(null_out);
    if (!started)
        return false;
    close_handle(info.hThread);
    out->process = info.hProcess;
    out->running = true;
    return true;
}

static bool platform_poll(process_handle *handle, process_result *out) {
    HANDLE process = handle->process;
    const DWORD state = WaitForSingleObject(process, 0);
    if (state == WAIT_TIMEOUT)
        return false;

    handle->running = false;
    if (state != WAIT_OBJECT_0) {
        close_handle(process);
        *out = failed_result();
        return true;
    }
    DWORD code = 0;
    const bool got = GetExitCodeProcess(process, &code) != 0;
    close_handle(process);
    *out = got ? (process_result){.exit_code = (int)code, .completed = true} : failed_result();
    return true;
}

static process_result platform_wait(process_handle *handle) {
    handle->running = false;
    return reap_handle(handle->process);
}

#else

/* --- POSIX --- */

/* Ends of a pipe, named so the code reads as data flow rather than indices. */
#define PIPE_READ 0
#define PIPE_WRITE 1

/* Turn a wait status into a result. */
static process_result translate(int status) {
    if (WIFEXITED(status))
        return (process_result){.exit_code = WEXITSTATUS(status), .completed = true};
    if (WIFSIGNALED(status))
        return (process_result){.exit_code = 128 + WTERMSIG(status), .completed = true};
    return failed_result();
}

/* Wait for `pid` and translate its status into an exit code. */
static process_result reap(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return failed_result();
    return translate(status);
}

/* Point the child's `fd` at /dev/null, so probe noise never reaches the user. */
static void silence(int fd) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
        dup2(null_fd, fd);
        close(null_fd);
    }
}

/* Give the child an stdin that is empty rather than the terminal, so a command
   that reads is answered with end of file instead of stealing the keyboard. */
static void empty_input(int fd) {
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
        dup2(null_fd, fd);
        close(null_fd);
    }
}

static process_result platform_run(const char *const argv[], const char *input, capture_kind what,
                                   char *out, size_t out_size) {
    int stdin_pipe[2];
    if (pipe(stdin_pipe) != 0)
        return failed_result();

    int capture_pipe[2] = {-1, -1};
    if (what != capture_none && pipe(capture_pipe) != 0) {
        close(stdin_pipe[PIPE_READ]);
        close(stdin_pipe[PIPE_WRITE]);
        return failed_result();
    }

    const int captured_fd = what == capture_err ? STDERR_FILENO : STDOUT_FILENO;
    const pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[PIPE_READ]);
        close(stdin_pipe[PIPE_WRITE]);
        if (what != capture_none) {
            close(capture_pipe[PIPE_READ]);
            close(capture_pipe[PIPE_WRITE]);
        }
        return failed_result();
    }

    if (pid == 0) {
        close(stdin_pipe[PIPE_WRITE]);
        dup2(stdin_pipe[PIPE_READ], STDIN_FILENO);
        close(stdin_pipe[PIPE_READ]);
        if (what == capture_none) {
            silence(STDOUT_FILENO);
            silence(STDERR_FILENO);
        } else {
            close(capture_pipe[PIPE_READ]);
            dup2(capture_pipe[PIPE_WRITE], captured_fd);
            close(capture_pipe[PIPE_WRITE]);
            silence(captured_fd == STDOUT_FILENO ? STDERR_FILENO : STDOUT_FILENO);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(EXIT_COMMAND_NOT_RUNNABLE);
    }

    close(stdin_pipe[PIPE_READ]);
    if (what != capture_none)
        close(capture_pipe[PIPE_WRITE]);
    if (input != NULL) {
        /* A short write only means the child stopped reading (it rejected the
           program early), which the exit status already reports. */
        const ssize_t written = write(stdin_pipe[PIPE_WRITE], input, strlen(input));
        (void)written;
    }
    close(stdin_pipe[PIPE_WRITE]); /* EOF, so the child stops waiting for input */

    if (what != capture_none) {
        /* Drain the stream before waiting: a child that fills the pipe would
           block forever if we waited first. */
        size_t total = 0;
        while (total + 1 < out_size) {
            const ssize_t got = read(capture_pipe[PIPE_READ], out + total, out_size - total - 1);
            if (got <= 0)
                break;
            total += (size_t)got;
        }
        out[total] = '\0';
        close(capture_pipe[PIPE_READ]);
    }
    return reap(pid);
}

static bool platform_start(const char *const argv[], process_handle *out) {
    const pid_t pid = fork();
    if (pid < 0)
        return false;

    if (pid == 0) {
        /* Nothing is fed in and nothing is read back: what this child does is
           observed elsewhere, by the file it writes. */
        empty_input(STDIN_FILENO);
        silence(STDOUT_FILENO);
        silence(STDERR_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(EXIT_COMMAND_NOT_RUNNABLE);
    }

    out->pid = pid;
    out->running = true;
    return true;
}

static bool platform_poll(process_handle *handle, process_result *out) {
    int status = 0;
    const pid_t finished = waitpid(handle->pid, &status, WNOHANG);
    if (finished == 0)
        return false; /* still going */

    handle->running = false;
    *out = finished < 0 ? failed_result() : translate(status);
    return true;
}

static process_result platform_wait(process_handle *handle) {
    handle->running = false;
    return reap(handle->pid);
}

#endif

/* --- the public half, which is the same on both --- */

process_result process_try(const char *const argv[], const char *input) {
    return platform_run(argv, input, capture_none, NULL, 0);
}

static process_result capture_stream(const char *const argv[], const char *input, capture_kind what,
                                     char *out, size_t out_size) {
    if (out == NULL || out_size == 0)
        return failed_result();
    out[0] = '\0';
    return platform_run(argv, input, what, out, out_size);
}

process_result process_capture(const char *const argv[], const char *input, char *out,
                               size_t out_size) {
    return capture_stream(argv, input, capture_out, out, out_size);
}

process_result process_capture_stderr(const char *const argv[], const char *input, char *out,
                                      size_t out_size) {
    return capture_stream(argv, input, capture_err, out, out_size);
}

bool process_start(const char *const argv[], process_handle *out) {
    memset(out, 0, sizeof *out);
    out->running = false;
    return platform_start(argv, out);
}

bool process_poll(process_handle *handle, process_result *out) {
    if (!handle->running) {
        *out = failed_result();
        return true;
    }
    return platform_poll(handle, out);
}

process_result process_wait(process_handle *handle) {
    if (!handle->running)
        return failed_result();
    return platform_wait(handle);
}

void process_pause_ms(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    const struct timespec pause = {.tv_sec = milliseconds / 1000,
                                   .tv_nsec = (long)(milliseconds % 1000) * 1000000L};
    (void)nanosleep(&pause, NULL);
#endif
}
