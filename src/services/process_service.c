#include <pickup/services/process_service.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Exit code a shell reports when a command cannot be executed. */
#define EXIT_COMMAND_NOT_RUNNABLE 127

/* Ends of a pipe, named so the code reads as data flow rather than indices. */
#define PIPE_READ  0
#define PIPE_WRITE 1

static process_result failed_result(void) {
    return (process_result){ .exit_code = -1, .completed = false };
}

/* Turn a wait status into a result. */
static process_result translate(int status) {
    if (WIFEXITED(status))
        return (process_result){ .exit_code = WEXITSTATUS(status), .completed = true };
    if (WIFSIGNALED(status))
        return (process_result){ .exit_code = WTERMSIG(status), .completed = true };
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
   that decides to read gets EOF instead of stealing the user's keystrokes. */
static void empty_input(int fd) {
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
        dup2(null_fd, fd);
        close(null_fd);
    }
}

/* Run `argv`, feeding `input` to its stdin and sending stdout to `out_fd`
   (or /dev/null when `out_fd` is negative). stderr is always silenced: a probe
   that fails is an answer, not something to report. */
static process_result spawn(const char *const argv[], const char *input, int out_fd) {
    int stdin_pipe[2];
    if (pipe(stdin_pipe) != 0)
        return failed_result();

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[PIPE_READ]);
        close(stdin_pipe[PIPE_WRITE]);
        return failed_result();
    }

    if (pid == 0) {
        close(stdin_pipe[PIPE_WRITE]);
        dup2(stdin_pipe[PIPE_READ], STDIN_FILENO);
        close(stdin_pipe[PIPE_READ]);
        if (out_fd >= 0)
            dup2(out_fd, STDOUT_FILENO);
        else
            silence(STDOUT_FILENO);
        silence(STDERR_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(EXIT_COMMAND_NOT_RUNNABLE);
    }

    close(stdin_pipe[PIPE_READ]);
    if (input != NULL) {
        size_t length = strlen(input);
        /* A short write only means the child stopped reading (it rejected the
           program early), which the exit status already reports. */
        ssize_t written = write(stdin_pipe[PIPE_WRITE], input, length);
        (void)written;
    }
    close(stdin_pipe[PIPE_WRITE]); /* EOF, so the child stops waiting for input */
    return reap(pid);
}

process_result process_try(const char *const argv[], const char *input) {
    return spawn(argv, input, -1);
}

bool process_start(const char *const argv[], process_handle *out) {
    out->pid = -1;
    out->running = false;

    pid_t pid = fork();
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

bool process_poll(process_handle *handle, process_result *out) {
    if (!handle->running) {
        *out = failed_result();
        return true;
    }

    int status = 0;
    pid_t finished = waitpid(handle->pid, &status, WNOHANG);
    if (finished == 0)
        return false; /* still going */

    handle->running = false;
    if (finished < 0) {
        *out = failed_result();
        return true;
    }
    *out = translate(status);
    return true;
}

process_result process_wait(process_handle *handle) {
    if (!handle->running)
        return failed_result();
    handle->running = false;
    return reap(handle->pid);
}

/* Run `argv` and read back whichever of the child's streams is `captured_fd`,
   silencing the other one. Both callers below want the same plumbing and differ
   only in which stream carries the answer they are after. */
static process_result capture_stream(const char *const argv[], const char *input,
                                     int captured_fd, char *out, size_t out_size) {
    if (out == NULL || out_size == 0)
        return failed_result();
    out[0] = '\0';

    int out_pipe[2];
    if (pipe(out_pipe) != 0)
        return failed_result();

    int stdin_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        close(out_pipe[PIPE_READ]);
        close(out_pipe[PIPE_WRITE]);
        return failed_result();
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[PIPE_READ]);
        close(out_pipe[PIPE_WRITE]);
        close(stdin_pipe[PIPE_READ]);
        close(stdin_pipe[PIPE_WRITE]);
        return failed_result();
    }

    if (pid == 0) {
        close(out_pipe[PIPE_READ]);
        close(stdin_pipe[PIPE_WRITE]);
        dup2(stdin_pipe[PIPE_READ], STDIN_FILENO);
        close(stdin_pipe[PIPE_READ]);
        dup2(out_pipe[PIPE_WRITE], captured_fd);
        close(out_pipe[PIPE_WRITE]);
        silence(captured_fd == STDOUT_FILENO ? STDERR_FILENO : STDOUT_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(EXIT_COMMAND_NOT_RUNNABLE);
    }

    close(out_pipe[PIPE_WRITE]);
    close(stdin_pipe[PIPE_READ]);
    if (input != NULL) {
        ssize_t written = write(stdin_pipe[PIPE_WRITE], input, strlen(input));
        (void)written;
    }
    close(stdin_pipe[PIPE_WRITE]);

    /* Drain the stream before waiting: a child that fills the pipe would block
       forever if we waited first. */
    size_t total = 0;
    while (total + 1 < out_size) {
        ssize_t got = read(out_pipe[PIPE_READ], out + total, out_size - total - 1);
        if (got <= 0)
            break;
        total += (size_t)got;
    }
    out[total] = '\0';
    close(out_pipe[PIPE_READ]);

    return reap(pid);
}

process_result process_capture(const char *const argv[], const char *input,
                               char *out, size_t out_size) {
    return capture_stream(argv, input, STDOUT_FILENO, out, out_size);
}

process_result process_capture_stderr(const char *const argv[], const char *input,
                                      char *out, size_t out_size) {
    return capture_stream(argv, input, STDERR_FILENO, out, out_size);
}
