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
 * compression itself, so the registry's zstd needs no telling — though whether
 * this tar can actually open one is a separate question, and asked below.
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

/* True if this tar can open a zstd-compressed archive, which is how the
   registry packs everything it publishes.

   Settled by opening one -- twenty-one bytes of empty archive -- rather than by
   asking. Neither question available answers it: `zstd --version` misses a tar
   with the library linked in and needing no such program, and
   `tar --zstd --version` misses a GNU tar that accepts the option and only
   fails when it tries to delegate. Worked out once. */
[[nodiscard]] bool archive_supports_zstd(void);

/* The command a caller should be told to install when archive_available is
   false. */
[[nodiscard]] const char *archive_requirement(void);

/* The command to install when archive_supports_zstd is false and tar itself is
   present: GNU tar hands zstd archives to this program. */
[[nodiscard]] const char *archive_zstd_requirement(void);

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

#endif /* PICKUP_ARCHIVE_SERVICE_H */
