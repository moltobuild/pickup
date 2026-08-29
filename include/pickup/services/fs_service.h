#ifndef PICKUP_FS_SERVICE_H
#define PICKUP_FS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return true if a filesystem entry exists at `path`. */
[[nodiscard]] bool fs_path_exists(const char *path);

/* Create a single directory. Succeeds if it already exists. */
[[nodiscard]] bool fs_make_dir(const char *path);

/* Write `content` to `path`, creating or truncating the file. */
[[nodiscard]] bool fs_write_file(const char *path, const char *content);

/* Read the whole file at `path` into a heap-allocated, NUL-terminated string.
   Returns NULL on error; the caller must free() the result. */
[[nodiscard]] char *fs_read_file(const char *path);

/* Read the whole file at `path`, bytes and all, into a heap-allocated buffer.
   `size` receives its length. Returns NULL on error; the caller must free().

   Distinct from fs_read_file because a compiled binary is not text: it holds
   NUL bytes, and a reader that stops at the first one would report a fraction
   of the file as the whole of it. */
[[nodiscard]] unsigned char *fs_read_bytes(const char *path, size_t *size);

/* Write `size` bytes to `path`, creating or truncating it, preserving the
   permissions it already had. An executable rewritten in place has to stay
   executable. */
[[nodiscard]] bool fs_write_bytes(const char *path, const unsigned char *data, size_t size);

/* Return true if `path` exists and is a directory. */
[[nodiscard]] bool fs_is_dir(const char *path);

/* Return true if `path` is a directory, without following a symlink to one.
   Walking into linked directories can loop forever when the links form a
   cycle, so a tree walk asks this instead of fs_is_dir. */
[[nodiscard]] bool fs_is_dir_no_follow(const char *path);

/* Compose a path into `out`. Returns false if the result did not fit, so a
   truncated path is reported instead of being used: two long source paths that
   truncate to the same object path would otherwise overwrite each other. */
[[nodiscard]] bool fs_format_path(char *out, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/* Create `path` and any missing parent directories. Succeeds if it exists. */
[[nodiscard]] bool fs_make_dirs(const char *path);

/* Recursively delete `path` and everything below it. Succeeds if `path` does
   not exist. Symlinks are removed, never followed, so a link inside the tree
   cannot lead the deletion outside of it. */
[[nodiscard]] bool fs_remove_tree(const char *path);

/* Modification time of `path` in nanoseconds since the epoch. Returns false if
   the file cannot be stat-ed. Filesystems without sub-second resolution simply
   report whole seconds. */
[[nodiscard]] bool fs_mtime_ns(const char *path, int64_t *out);

/* Size of `path` in bytes. False if it cannot be stat-ed, which for a download
   in flight simply means it has not been created yet. */
[[nodiscard]] bool fs_file_size(const char *path, long long *out);

/* Total size of everything under `path`, following no symlinks, so a link into
   a tree cannot make it count twice. Zero for a path that does not exist yet,
   which is what a directory being filled looks like at the start. */
[[nodiscard]] bool fs_tree_size(const char *path, long long *out);

/* Rename `from` to `to`, replacing anything already there. Within one
   filesystem this is atomic, which is what lets a half-written directory be
   assembled out of the way and only then take its final name. */
[[nodiscard]] bool fs_rename(const char *from, const char *to);

/* Return true if `target` must be rebuilt from `source`: true when `target`
   is missing or `source` has a newer modification time (nanosecond precision,
   so two edits within the same second are still told apart). */
[[nodiscard]] bool fs_source_newer(const char *source, const char *target);

/* The canonical form of `path`: symlinks followed, `.` and `..` resolved,
   written into `out`.
 *
 * Here rather than at the six places that used to call `realpath` directly,
 * because `realpath` is POSIX and the callers are not: keeping the platform
 * inside this service is what lets the rest of the tree stay one
 * implementation (RFC-0017).
 *
 * False when the path cannot be resolved — which for a symlink means a broken
 * one — or when the answer does not fit. Callers that treat "unresolvable" as
 * "use what I gave you" say so themselves; this reports rather than guesses. */
[[nodiscard]] bool fs_real_path(const char *path, char *out, size_t size);

/* The name a file would be run by, with whatever the platform appends to an
 * executable taken off.
 *
 * On POSIX that is the name itself: any file can carry the execute bit, and
 * nothing is added to its name to say so. On Windows the suffix *is* the
 * permission — a file called `gcc` cannot be run and `gcc.exe` can — so `.exe`
 * is both required here and stripped, which is the filter that `access(X_OK)`
 * provides on POSIX and cannot provide there, since Windows has no execute bit
 * for it to read.
 *
 * False when the file cannot be run by name on this platform, or when the
 * answer does not fit. */
[[nodiscard]] bool fs_executable_name(const char *file, char *out, size_t size);

#endif /* PICKUP_FS_SERVICE_H */
