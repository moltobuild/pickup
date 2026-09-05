#include <pickup/services/archive_service.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/process_service.h>
#include <pickup/util/progress.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

/* Stops GNU tar reading an archive name as `host:path`. Only GNU tar has it,
   and only GNU tar needs it; whether this one does is settled by asking, in
   archive_supports_force_local. */
#define ARG_FORCE_LOCAL "--force-local"

/* Room for the composed --strip-components argument. */
#define STRIP_ARG_SIZE 32

const char *archive_requirement(void) { return TAR_COMMAND; }

const char *archive_zstd_requirement(void) { return "zstd"; }

const char *archive_xz_requirement(void) { return "xz"; }

const char *archive_gzip_requirement(void) { return "gzip"; }

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

bool archive_supports_force_local(void) {
    static bool checked = false;
    static bool supported = false;
    if (checked)
        return supported;

    /* Paired with --version for the same reason --wildcards is: tar parses its
       options, finds nothing to do, and says whether it recognised the flag. */
    const char *argv[] = {TAR_COMMAND, ARG_FORCE_LOCAL, ARG_VERSION, NULL};
    process_result result = process_try(argv, NULL);
    supported = result.completed && result.exit_code == 0;
    checked = true;
    return supported;
}

/*
 * An empty tar, packed three ways. Each was generated with
 *
 *   tar -cf - --files-from /dev/null | <compressor>
 *
 * and each is the whole probe for the codec that produced it: listing one
 * settles whether this tar opens that packing, for a temporary file and a
 * process.
 */

/* zstd -19. Twenty-one bytes. */
static const unsigned char empty_tar_zst[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x68, 0x45, 0x00, 0x00, 0x08, 0x00,
    0x01, 0x00, 0xfc, 0x87, 0x07, 0x42, 0xbc, 0xbd, 0x7e, 0xd6,
};

/* xz -9. */
static const unsigned char empty_tar_xz[] = {
    0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x04, 0xe6, 0xd6, 0xb4, 0x46, 0x04, 0xc0, 0x34, 0x80,
    0x50, 0x21, 0x01, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xec, 0xb2, 0xb9, 0x70,
    0xe0, 0x27, 0xff, 0x00, 0x2c, 0x5d, 0x00, 0x00, 0x6f, 0xfd, 0xff, 0xff, 0xa3, 0xb7, 0xff, 0x47,
    0x3e, 0x48, 0x15, 0x72, 0x39, 0x61, 0x51, 0xb8, 0x92, 0x28, 0xe6, 0xa3, 0x86, 0x07, 0xf9, 0xee,
    0xe4, 0x1e, 0x82, 0xd3, 0x2f, 0xc5, 0x3a, 0x3c, 0x01, 0x4b, 0xb1, 0x7e, 0xc9, 0x8a, 0x5c, 0x32,
    0x1b, 0x64, 0x00, 0x00, 0xaf, 0x18, 0x12, 0xb3, 0x2f, 0x55, 0xcc, 0x33, 0x00, 0x01, 0x50, 0x80,
    0x50, 0x00, 0x00, 0x00, 0x1a, 0x63, 0x30, 0xb5, 0xb1, 0xc4, 0x67, 0xfb, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x59, 0x5a,
};

/* gzip -9 -n. Forty-five bytes, and -n so there is no date inside the blob. */
static const unsigned char empty_tar_gz[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xed, 0xc1, 0x01, 0x0d, 0x00, 0x00,
    0x00, 0xc2, 0xa0, 0xf7, 0x4f, 0x6d, 0x0e, 0x37, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x37, 0x03, 0x9a, 0xde, 0x1d, 0x27, 0x00, 0x28, 0x00, 0x00,
};

/* What a codec question has answered, once. */
typedef struct {
    bool checked;
    bool supported;
} codec_answer;

/*
 * Whether this tar opens an archive packed this way.
 *
 * The probe does rather than asks, because neither question answers it:
 * `zstd --version` misses a bsdtar with the library linked in, and
 * `tar --zstd --version` misses a GNU tar that accepts the option and only
 * fails later, when it tries to hand the archive to a program that is not
 * there. Opening one settles both.
 *
 * What it does not settle is a filter that opens a small archive and hangs on
 * a large one, which is what libarchive's outside-program path does on
 * Windows: it fills the child's stdin and waits on its stdout, and past a pipe
 * buffer of compressed input neither side moves again. No probe small enough
 * to embed can see that, so this does not pretend to -- archive_extract_
 * selected watches for it instead, where the volume is real.
 */
static bool codec_opens(codec_answer *answer, const char *stem, const unsigned char *bytes,
                        size_t size) {
    if (answer->checked)
        return answer->supported;
    answer->checked = true;

    if (!archive_available())
        return answer->supported;

    char path[PICKUP_PATHS_MAX];
    if (!fs_temp_file(stem, path, sizeof path))
        return answer->supported;

    /* Written through the same call the rest of the code uses: the probe must
       fail for the same reasons a real extraction would. */
    if (fs_write_bytes(path, bytes, size)) {
        /* The probe reads a temporary file by absolute path, which is where
           the remote-name reading bites first: without the flag this answers
           "no" on Windows for a tar that handles the codec perfectly well. */
        const char *argv[] = {TAR_COMMAND, ARG_FORCE_LOCAL, ARG_LIST, path, NULL};
        const char *plain[] = {TAR_COMMAND, ARG_LIST, path, NULL};
        process_result result = process_try(archive_supports_force_local() ? argv : plain, NULL);
        answer->supported = result.completed && result.exit_code == 0;
    }

    (void)remove(path);
    return answer->supported;
}

bool archive_supports_zstd(void) {
    static codec_answer answer;
    return codec_opens(&answer, "pickup_zstd", empty_tar_zst, sizeof empty_tar_zst);
}

bool archive_supports_xz(void) {
    static codec_answer answer;
    return codec_opens(&answer, "pickup_xz", empty_tar_xz, sizeof empty_tar_xz);
}

bool archive_supports_gzip(void) {
    static codec_answer answer;
    return codec_opens(&answer, "pickup_gzip", empty_tar_gz, sizeof empty_tar_gz);
}

/*
 * Whether installing the program a tar delegates a codec to is a remedy.
 *
 * On a Unix it is: GNU tar pipes the archive through gzip, xz or zstd, and the
 * pipe works. On Windows it is not, for the tar that will actually run.
 *
 * Which tar that is, is not the user's choice. Pickup starts it through
 * CreateProcess with a bare name, and that search reaches the system directory
 * before it reaches PATH -- so C:\Windows\System32\tar.exe is what runs, no
 * matter what else is installed or which shell the command was typed in. That
 * is bsdtar, linked against zlib alone, and its outside-program path fills the
 * child's stdin while waiting on the child's stdout: past a pipe buffer of
 * compressed input, neither side moves again.
 *
 * So on Windows "install xz" does not fix a thing. It turns a clear refusal
 * into a hang, which is a worse place to be sent.
 */
bool archive_delegation_works(void) {
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}

bool archive_extract(const char *archive, const char *destination, int strip_components) {
    const archive_request request = {.strip_components = strip_components};
    return archive_extract_selected(archive, destination, &request);
}

#define ARG_EXCLUDE_FORMAT "--exclude=%s"

/*
 * Fixed part of the command, plus room for the exclusions and patterns.
 *
 * Nine are named: tar, --force-local, -xf, the archive, -C, the destination,
 * --strip-components, --wildcards, and the NULL that ends the list. The tenth
 * is `push`'s own margin — it stops at `count + 1 < ARGV_MAX`, so an array of
 * N holds N-1 entries, and the one it refuses at full load is the NULL. An
 * argv without it is not a shorter command, it is a command whose end the
 * kernel reads out of whatever follows in memory.
 *
 * Worth stating because the worst case is reachable: 24 exclusions and 24
 * patterns are both inside what `build_command` accepts.
 */
#define ARGV_FIXED 10
#define ARGV_MAX (ARGV_FIXED + 2 * ARCHIVE_MAX_PATTERNS)

/* Composed --exclude= arguments, which have to outlive the argv holding them. */
#define EXCLUDE_ARG_SIZE 256

/* How often the child is looked in on while it works. */
#define POLL_INTERVAL_MS 120

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
    /* Before -f, because it governs how the name after it is read. */
    if (archive_supports_force_local())
        push(command, ARG_FORCE_LOCAL);
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

static void wait_a_moment(void) { process_pause_ms(POLL_INTERVAL_MS); }

/* What has landed in the destination so far, which is the only measure of an
   extraction there is: tar says nothing while it works. */
static long long produced_so_far(const char *destination) {
    long long produced = 0;
    if (!fs_tree_size(destination, &produced))
        produced = 0;
    return produced;
}

/*
 * How long the destination may not grow before the extraction is called wedged.
 *
 * Not a limit on how long an extraction may take -- a gigabyte of compiler out
 * of a compressed stream takes minutes, and those minutes are work. What is
 * measured is nothing happening at all.
 *
 * Two and a half minutes because there is one honest reason for a long silence:
 * a selective extraction decompresses its way to the members it was asked for
 * and writes nothing until it reaches them, which on a release where most of
 * the content is skipped runs to about ninety seconds. And because the failure
 * this catches does not resolve itself -- libarchive delegating a codec to
 * another program on Windows fills that program's stdin, waits on its stdout
 * and stays there -- so a generous limit costs nothing that was going to
 * finish, and turns a hang with no end into a sentence.
 */
#define STALL_LIMIT_MS 150000u

/* Show that it is still working, and how much has come out so far.

   There is no honest percentage to give. What tar reads is not the archive but
   the decompressed stream handed to it by xz, so measuring its progress against
   the size of the file on disk compares 11 GB against 1.8 GB and reports 600%.
   The total unpacked size is not known in advance either, and finding it out
   costs a second pass over the whole archive. So: a spinner, and the bytes that
   have actually landed. */
static void draw(const archive_request *request, size_t frame, long long produced) {
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

archive_outcome archive_extract_reported(const char *archive, const char *destination,
                                         const archive_request *request) {
    if (!archive_available())
        return archive_failed;

    tar_command command;
    if (!build_command(&command, archive, destination, request))
        return archive_failed;

    /* Started and watched even when there is nothing to draw. The watching is
       not for the spinner's sake: a child that has stopped moving has to be
       noticed on a build server too, where the alternative is a job that hangs
       until something outside kills it. */
    bool show_progress = request->label != NULL && progress_is_interactive(stdout);

    process_handle handle;
    if (!process_start(command.argv, &handle))
        return archive_failed;

    long long produced = 0;
    long long last_produced = -1;
    unsigned still_ms = 0;
    bool stopped = false;

    process_result result;
    for (size_t frame = 0; !process_poll(&handle, &result); frame++) {
        produced = produced_so_far(destination);
        if (produced != last_produced) {
            last_produced = produced;
            still_ms = 0;
        } else if (!stopped && (still_ms += POLL_INTERVAL_MS) >= STALL_LIMIT_MS) {
            process_stop(&handle);
            stopped = true;
        }

        if (show_progress)
            draw(request, frame, produced);
        wait_a_moment();
    }

    /* A stopped child is not judged on its exit status. It was killed, so it
       has one, and for a selective extraction `succeeded` would read the bytes
       that did land as success. */
    bool ok = !stopped && succeeded(&result, request, destination);
    if (show_progress) {
        /* Leave the final size on screen rather than whatever the last poll
           happened to see. */
        if (ok)
            draw(request, 0, produced_so_far(destination));
        progress_done(stdout);
    }

    if (stopped)
        return archive_stalled;
    return ok ? archive_ok : archive_failed;
}

bool archive_extract_selected(const char *archive, const char *destination,
                              const archive_request *request) {
    return archive_extract_reported(archive, destination, request) == archive_ok;
}
