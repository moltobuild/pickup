#ifndef PICKUP_ZIP_H
#define PICKUP_ZIP_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Just enough of the zip format to reach inside a `.conda` package.
 *
 * A conda package is a zip holding two tarballs — the payload and the metadata
 * — and the entries are **stored**, not deflated: the compression already
 * happened inside the tarballs, so the zip is a container and nothing more.
 * That is what makes this worth writing rather than avoiding. Locating an entry
 * means walking local file headers and adding up lengths; no decompression is
 * involved, so there is no compression library to link and no `unzip` to
 * require on the machine.
 *
 * Only stored entries are understood. A deflated one is refused rather than
 * silently mishandled: the point of reading the format by hand is that the
 * layout is simple, and an entry that is not stored means the assumption no
 * longer holds.
 *
 * Sizes are read from the zip64 extra field when the 32-bit fields say to. A
 * conda package that carries an 85 MB compiler needs it, so this is the normal
 * path rather than a fallback.
 */

/* Longest member name read. Conda names them after the package. */
#define ZIP_NAME_MAX 512

typedef struct {
    char name[ZIP_NAME_MAX];
    long long offset;  /* where the member's bytes begin in the file */
    long long size;    /* how many of them there are */
} zip_member;

/* Find the first stored member whose name begins with `prefix`.

   Matched by prefix because conda names its members after the package and its
   version — `pkg-gcc_impl_linux-64-16.1.0-h5fcb69b_1.tar.zst` — so the only
   stable part is what comes first. False when there is no such member, or the
   file is not a zip Pickup can read. */
[[nodiscard]] bool zip_find_member(const char *path, const char *prefix, zip_member *out);

/* Copy `member` out of `path` into a file at `destination`.

   A copy rather than a view: what receives it is `tar`, a separate process that
   needs a file of its own, and the alternative — handing it a descriptor
   positioned mid-file — would leave it reading whatever follows the member. */
[[nodiscard]] bool zip_extract_member(const char *path, const zip_member *member,
                                      const char *destination);

#endif /* PICKUP_ZIP_H */
