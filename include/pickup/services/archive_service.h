#ifndef PICKUP_ARCHIVE_SERVICE_H
#define PICKUP_ARCHIVE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Unpacking downloaded toolchains.
 *
 * Delegated to the system's tar for the same reason downloads are delegated to
 * curl: it keeps Pickup building against libc alone, and tar is present on
 * every target platform, Windows 10 and later included. Modern tar detects the
 * compression itself, so no packing needs telling — though whether this tar can
 * actually open a given one is a separate question, and asked below, once per
 * codec. Being present is not being capable: the tar Windows ships is linked
 * against zlib alone and hands everything else to a program that is usually
 * not there.
 */

/* True if a usable tar was found. Worked out once. */
[[nodiscard]] bool archive_available(void);

/* True if this tar takes --wildcards.

   GNU tar needs telling before it will treat the requested members as globs;
   its wildcards-by-default applies to exclusions only. bsdtar, which macOS and
   Windows ship, globs member arguments already and has no such option, so
   passing it there is an unknown-option error.

   Which one is present is settled by running tar rather than by reading its
   version string, the same way a compiler is asked whether it takes a `-std=`
   instead of being looked up in a table. Worked out once. */
[[nodiscard]] bool archive_supports_wildcards(void);

/* True if this tar takes --force-local.

   Without it, GNU tar reads an archive name as `host:path` whenever the part
   before the first colon holds no slash — which is every absolute path on
   Windows. `C:\toolchains\x.tar.zst` is not a file to that tar, it is a
   request to reach a machine called `C`, and it answers `Cannot connect to C:
   resolve failed`. Nothing about the path is wrong; the reading is.

   Settled by asking, like the wildcards question above, because bsdtar has no
   such option and needs none: it never treats a name as remote. Worked out
   once. */
[[nodiscard]] bool archive_supports_force_local(void);

/* True if this tar can open an archive packed this way -- one question per
   codec the registry publishes.

   Settled by opening one, an empty archive of a few dozen bytes, rather than
   by asking. Neither question available answers it: `zstd --version` misses a
   tar with the library linked in and needing no such program, and
   `tar --zstd --version` misses a GNU tar that accepts the option and only
   fails when it tries to delegate. Each is worked out once.

   Note what an answer of true does *not* promise. A tar that opens the probe
   may still be one that delegates the codec to another program, and on Windows
   that path deadlocks once the compressed input passes a pipe buffer -- so a
   yes here means "packed this way is worth attempting", and the attempt itself
   is watched. */
[[nodiscard]] bool archive_supports_zstd(void);
[[nodiscard]] bool archive_supports_xz(void);
[[nodiscard]] bool archive_supports_gzip(void);

/* The command a caller should be told to install when archive_available is
   false. */
[[nodiscard]] const char *archive_requirement(void);

/* The program a tar without the codec built in delegates to, which is what a
   caller has to be told to install when the matching question answers false.
   Naming it is honest only where tar delegates and the program is missing; a
   tar that cannot use the program either is a different problem, and the
   remedy for that one is a different tar. */
[[nodiscard]] const char *archive_zstd_requirement(void);
[[nodiscard]] const char *archive_xz_requirement(void);
[[nodiscard]] const char *archive_gzip_requirement(void);

/* Whether naming that program is a remedy at all.

   A tar without a codec built in delegates it, and where the delegation works
   installing the program is the fix. Where the delegation itself is what is
   broken -- libarchive on Windows, which deadlocks against the program it
   started -- it is not, and saying so sends someone to fetch something that
   turns a refusal into a hang. */
[[nodiscard]] bool archive_delegation_works(void);

/* Extract `archive` into `destination`, which must already exist.

   `strip_components` drops that many leading path components, for an archive
   built around a directory named after its release. What the registry publishes
   needs none dropped: its `bin` and `lib` are already at the top. */
[[nodiscard]] bool archive_extract(const char *archive, const char *destination,
                                   int strip_components);

/* Most patterns and exclusions one extraction may name. */
#define ARCHIVE_MAX_PATTERNS 24

/* What an extraction selects, and what it reports while it runs. */
typedef struct {
    const char *const *patterns; /* entries to extract; NULL for all of them */
    size_t pattern_count;
    const char *const *excludes; /* entries to skip even if they match */
    size_t exclude_count;
    int strip_components;
    const char *label;         /* shown while extracting; NULL to stay quiet */
    const char *waiting_label; /* shown until the first entry lands */
} archive_request;

/* Extract the entries of `archive` that `request` selects into `destination`.

   Only what matches is ever written: the rest of the archive is decompressed
   and discarded as it streams past, never unpacked and deleted afterwards. On
   a release that is mostly things Pickup has no use for, that is the difference
   between needing 11 GB of free space and needing half of one.

   Patterns are shell globs matched against the whole path inside the archive,
   so they usually open with a star and a slash, to step over the archive's own
   top-level directory.

   Progress is drawn when `label` is set and the output is a terminal: a spinner
   and the bytes unpacked so far. Unpacking is minutes of silence otherwise, and
   silence is what makes people think it has hung. */
[[nodiscard]] bool archive_extract_selected(const char *archive, const char *destination,
                                            const archive_request *request);

/* How an extraction ended, for a caller that reports the difference.

   `archive_stalled` is its own answer rather than a kind of failure: a tar that
   produced nothing for long enough to be stopped has not read a bad archive, it
   has wedged -- on Windows, by handing the codec to a program and then
   deadlocking against it -- and telling someone their download is corrupt would
   send them to fix the wrong thing. */
typedef enum {
    archive_ok,
    archive_failed,
    archive_stalled,
} archive_outcome;

[[nodiscard]] archive_outcome archive_extract_reported(const char *archive,
                                                       const char *destination,
                                                       const archive_request *request);

#endif /* PICKUP_ARCHIVE_SERVICE_H */
