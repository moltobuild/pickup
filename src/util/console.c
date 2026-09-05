#include <pickup/util/console.h>

#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>

/* Declared by the SDK since Windows 10, and not by the older headers mingw may
   be built against. The value is the mode bit itself, so defining it where it
   is missing costs nothing and lets one binary run on both. */
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static bool sequences = false;
static UINT original_code_page = 0;

/* Ask one handle for the mode that makes it read escape sequences.
 *
 * A redirected stream is not a console and GetConsoleMode fails on it, which
 * is not a failure to report: nothing is drawn to a pipe anyway, and whether
 * colour is wanted there is already decided by isatty. */
static bool enable_sequences(DWORD stream) {
    HANDLE handle = GetStdHandle(stream);
    if (handle == NULL || handle == INVALID_HANDLE_VALUE)
        return false;

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode))
        return false;
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
        return true;
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

/* The console outlives the process, so the page it had is the page it keeps. */
static void restore_code_page(void) {
    if (original_code_page != 0)
        (void)SetConsoleOutputCP(original_code_page);
}

void console_configure(void) {
    /* Both streams, because they are drawn on by different commands: the
       download bar goes to stdout and the probing bar to stderr. */
    bool out = enable_sequences(STD_OUTPUT_HANDLE);
    bool err = enable_sequences(STD_ERROR_HANDLE);
    sequences = out || err;

    original_code_page = GetConsoleOutputCP();
    if (original_code_page != CP_UTF8 && SetConsoleOutputCP(CP_UTF8))
        (void)atexit(restore_code_page);
    else
        original_code_page = 0;
}

bool console_supports_sequences(void) { return sequences; }

#else

/* A terminal acts on sequences and reads UTF-8. There is nothing to arrange. */
void console_configure(void) {}

bool console_supports_sequences(void) { return true; }

#endif
