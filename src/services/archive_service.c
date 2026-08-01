#include <pickup/services/archive_service.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/process_service.h>
#include <pickup/util/progress.h>

#include <stdio.h>
#include <time.h>

/* The extractor Pickup drives.
   -x extract, -f from a file, -C into a directory */
#define TAR_COMMAND    "tar"
#define ARG_EXTRACT    "-xf"
#define ARG_DIRECTORY  "-C"
#define ARG_STRIP_FORMAT "--strip-components=%d"
#define ARG_VERSION    "--version"

/* Room for the composed --strip-components argument. */
#define STRIP_ARG_SIZE 32

const char *archive_requirement(void) {
    return TAR_COMMAND;
}

bool archive_available(void) {
    static bool checked = false;
    static bool available = false;
    if (checked)
        return available;

    const char *argv[] = { TAR_COMMAND, ARG_VERSION, NULL };
    process_result result = process_try(argv, NULL);
    available = result.completed && result.exit_code == 0;
    checked = true;
    return available;
}

bool archive_extract(const char *archive, const char *destination, int strip_components) {
    const archive_request request = { .strip_components = strip_components };
    return archive_extract_selected(archive, destination, &request);
}

/* Treat the member arguments as globs. GNU tar needs telling; bsdtar, which is
   what macOS and Windows ship, globs by default and rejects the flag. Only
   passed when there are patterns to match, so the plain full extraction keeps
   working everywhere. */
#define ARG_WILDCARDS "--wildcards"
#define ARG_EXCLUDE_FORMAT "--exclude=%s"

/* Fixed part of the command, plus room for the exclusions and patterns. */
#define ARGV_FIXED 8
#define ARGV_MAX   (ARGV_FIXED + 2 * ARCHIVE_MAX_PATTERNS)

/* Composed --exclude= arguments, which have to outlive the argv holding them. */
#define EXCLUDE_ARG_SIZE 256

/* How often the child is looked in on while it works. */
#define POLL_INTERVAL_NS 120000000L /* 0.12 s */

typedef struct {
    const char *argv[ARGV_MAX];
    char excludes[ARCHIVE_MAX_PATTERNS][EXCLUDE_ARG_SIZE];
    char strip[STRIP_ARG_SIZE];
    size_t count;
} tar_command;

static void push(tar_command *command, const char *argument) {
    if (command->count + 1 < ARGV_MAX)
        command->argv[command->count++] = argument;
}

/* Build `tar -xf <archive> -C <destination> --strip-components=N [...]`. */
static bool build_command(tar_command *command, const char *archive,
                          const char *destination, const archive_request *request) {
    if (request->pattern_count > ARCHIVE_MAX_PATTERNS
        || request->exclude_count > ARCHIVE_MAX_PATTERNS)
        return false;

    command->count = 0;
    snprintf(command->strip, sizeof command->strip, ARG_STRIP_FORMAT,
             request->strip_components);

    push(command, TAR_COMMAND);
    push(command, ARG_EXTRACT);
    push(command, archive);
    push(command, ARG_DIRECTORY);
    push(command, destination);
    push(command, command->strip);

    for (size_t i = 0; i < request->exclude_count; i++) {
        snprintf(command->excludes[i], EXCLUDE_ARG_SIZE, ARG_EXCLUDE_FORMAT,
                 request->excludes[i]);
        push(command, command->excludes[i]);
    }

    if (request->pattern_count > 0) {
        push(command, ARG_WILDCARDS);
        for (size_t i = 0; i < request->pattern_count; i++)
            push(command, request->patterns[i]);
    }

    push(command, NULL);
    return true;
}

static void wait_a_moment(void) {
    struct timespec pause = { .tv_sec = 0, .tv_nsec = POLL_INTERVAL_NS };
    nanosleep(&pause, NULL);
}

/* Show that it is still working, and how much has come out so far.

   There is no honest percentage to give. What tar reads is not the archive but
   the decompressed stream handed to it by xz, so measuring its progress against
   the size of the file on disk compares 11 GB against 1.8 GB and reports 600%.
   The total unpacked size is not known in advance either, and finding it out
   costs a second pass over the whole archive. So: a spinner, and the bytes that
   have actually landed. */
static void draw(const archive_request *request, const char *destination, size_t frame) {
    long long produced = 0;
    if (!fs_tree_size(destination, &produced))
        produced = 0;

    /* Before the first entry lands, tar is decompressing its way to the parts
       that were asked for and writing nothing. On a release where most of the
       content is skipped that is a minute and a half of a byte counter reading
       zero, which looks like a stall rather than like work. Saying what is
       happening costs a label. */
    if (produced == 0 && request->waiting_label != NULL) {
        spinner_wait(stdout, request->waiting_label, frame);
        return;
    }
    spinner_draw(stdout, request->label, frame, produced);
}

/* Did the extraction deliver?

   When the whole archive was asked for, tar's exit status is the answer. When
   only a selection was, it is not: tar reports failure if any named pattern
   matched nothing, and a pattern matching nothing is normal here — a release
   that ships no lld should still install the compiler it does ship. So the
   question becomes whether anything was extracted at all. */
static bool succeeded(const process_result *result, const archive_request *request,
                      const char *destination) {
    if (!result->completed)
        return false;
    if (request->pattern_count == 0)
        return result->exit_code == 0;

    long long produced = 0;
    return fs_tree_size(destination, &produced) && produced > 0;
}

bool archive_extract_selected(const char *archive, const char *destination,
                              const archive_request *request) {
    if (!archive_available())
        return false;

    tar_command command;
    if (!build_command(&command, archive, destination, request))
        return false;

    bool show_progress = request->label != NULL && progress_is_interactive(stdout);
    if (!show_progress) {
        process_result result = process_try(command.argv, NULL);
        return succeeded(&result, request, destination);
    }

    process_handle handle;
    if (!process_start(command.argv, &handle))
        return false;

    process_result result;
    for (size_t frame = 0; !process_poll(&handle, &result); frame++) {
        draw(request, destination, frame);
        wait_a_moment();
    }

    bool ok = succeeded(&result, request, destination);
    /* Leave the final size on screen rather than whatever the last poll saw. */
    if (ok)
        draw(request, destination, 0);
    progress_done(stdout);

    return ok;
}
