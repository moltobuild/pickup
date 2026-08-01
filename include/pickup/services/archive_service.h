#ifndef PICKUP_ARCHIVE_SERVICE_H
#define PICKUP_ARCHIVE_SERVICE_H

#include <stdbool.h>

/*
 * Unpacking downloaded toolchains.
 *
 * Delegated to the system's tar for the same reason downloads are delegated to
 * curl: it keeps Pickup building against libc alone, and tar is present on
 * every target platform, Windows 10 and later included. Modern tar detects the
 * compression itself, which matters because LLVM publishes .tar.xz for most
 * platforms and .tar.gz for others.
 */

/* True if a usable tar was found. Worked out once. */
[[nodiscard]] bool archive_available(void);

/* The command a caller should be told to install when archive_available is
   false. */
[[nodiscard]] const char *archive_requirement(void);

/* Extract `archive` into `destination`, which must already exist.

   `strip_components` drops that many leading path components, which is how the
   single top-level directory these archives carry is removed so that `bin` and
   `lib` land directly in the destination. */
[[nodiscard]] bool archive_extract(const char *archive, const char *destination,
                                   int strip_components);

#endif /* PICKUP_ARCHIVE_SERVICE_H */
