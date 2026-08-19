#include <pickup/services/archive_service.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/process_service.h>
#include <pickup/util/progress.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* The extractor Pickup drives.
   -x extract, -f from a file, -C into a directory */
#define TAR_COMMAND "tar"
#define ARG_EXTRACT "-xf"
#define ARG_DIRECTORY "-C"
#define ARG_STRIP_FORMAT "--strip-components=%d"
#define ARG_VERSION "--version"

/* Asks for the member arguments to be read as globs. Only GNU tar has it, and
   only GNU tar needs it; whether this one does is settled by asking, in
   archive_supports_wildcards. */
#define ARG_WILDCARDS "--wildcards"

/* Room for the composed --strip-components argument. */
#define STRIP_ARG_SIZE 32

const char *archive_requirement(void) { return TAR_COMMAND; }

const char *archive_zstd_requirement(void) { return "zstd"; }

bool archive_available(void) {
    static bool checked = false;
    static bool available = false;
    if (checked)
        return available;

    const char *argv[] = {TAR_COMMAND, ARG_VERSION, NULL};
    process_result result = process_try(argv, NULL);
    available = result.completed && result.exit_code == 0;
    checked = true;
    return available;
}

bool archive_supports_wildcards(void) {
    static bool checked = false;
    static bool supported = false;
    if (checked)
        return supported;

    /* Paired with --version so the question costs nothing: tar parses its
       options, finds nothing to do, and says whether it recognised the flag.
       An implementation that does not have it exits non-zero without touching
       an archive. */
    const char *argv[] = {TAR_COMMAND, ARG_WILDCARDS, ARG_VERSION, NULL};
    process_result result = process_try(argv, NULL);
    supported = result.completed && result.exit_code == 0;
    checked = true;
    return supported;
}

/* -t list, -f from a file: the cheapest thing that still opens an archive. */
#define ARG_LIST "-tf"

/* Where the probe archive is written. */
#define ZSTD_PROBE_TEMPLATE "/tmp/pickup_zstd_XXXXXX"

/*
 * An empty tar, compressed with zstd. Generated with:
 *
 *   tar -cf - --files-from /dev/null | zstd -19
 *
 * Twenty-one bytes, and listing it is the whole probe.
 */
static const unsigned char empty_tar_zst[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x68, 0x45, 0x00, 0x00, 0x08, 0x00,
    0x01, 0x00, 0xfc, 0x87, 0x07, 0x42, 0xbc, 0xbd, 0x7e, 0xd6,
};

/*
 * Whether this tar can open what the registry packs.
 *
 * The probe does rather than asks, because neither question answers it:
 * `zstd --version` misses a bsdtar with the library linked in, and
 * `tar --zstd --version` misses a GNU tar that accepts the option and only
 * fails later, when it tries to hand the archive to a program that is not
 * there. Opening one settles both.
 */
bool archive_supports_zstd(void) {
    static bool checked = false;
    static bool supported = false;
    if (checked)
        return supported;
    checked = true;

    if (!archive_available())
        return supported;

    char path[] = ZSTD_PROBE_TEMPLATE;
    int descriptor = mkstemp(path);
    if (descriptor < 0)
        return supported;
    (void)close(descriptor);

    /* Written through the same call the rest of the code uses: the probe must
       fail for the same reasons a real extraction would. */
    if (fs_write_bytes(path, empty_tar_zst, sizeof empty_tar_zst)) {
        const char *argv[] = {TAR_COMMAND, ARG_LIST, path, NULL};
        process_result result = process_try(argv, NULL);
        supported = result.completed && result.exit_code == 0;
    }

    (void)remove(path);
    return supported;
}

bool archive_extract(const char *archive, const char *destination, int strip_components) {
    const archive_request request = {.strip_components = strip_components};
    return archive_extract_selected(archive, destination, &request);
}

#define ARG_EXCLUDE_FORMAT "--exclude=%s"

/* Fixed part of the command, plus room for the exclusions and patterns. */
#define ARGV_FIXED 8
#define ARGV_MAX (ARGV_FIXED + 2 * ARCHIVE_MAX_PATTERNS)

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
static bool build_command(tar_command *command, const char *archive, const char *destination,
                          const archive_request *request) {
    if (request->pattern_count > ARCHIVE_MAX_PATTERNS ||
        request->exclude_count > ARCHIVE_MAX_PATTERNS)
        return false;

    command->count = 0;
    snprintf(command->strip, sizeof command->strip, ARG_STRIP_FORMAT, request->strip_components);

    push(command, TAR_COMMAND);
    push(command, ARG_EXTRACT);
    push(command, archive);
    push(command, ARG_DIRECTORY);
    push(command, destination);
    push(command, command->strip);

    /* Exclusions first, and they need no flag: both implementations already
       read those as globs. */
    for (size_t i = 0; i < request->exclude_count; i++) {
        snprintf(command->excludes[i], EXCLUDE_ARG_SIZE, ARG_EXCLUDE_FORMAT, request->excludes[i]);
        push(command, command->excludes[i]);
    }

    if (request->pattern_count > 0) {
        /* In GNU tar a matching option governs the patterns that follow it, so
           this belongs here rather than earlier. Passed only where it is
           understood, and only when there are members to match: a full
           extraction names none. */
        if (archive_supports_wildcards())
            push(command, ARG_WILDCARDS);
        for (size_t i = 0; i < request->pattern_count; i++)
            push(command, request->patterns[i]);
    }

    push(command, NULL);
    return true;
}

static void wait_a_moment(void) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = POLL_INTERVAL_NS};
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
