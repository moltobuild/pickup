#ifndef PICKUP_FORMAT_H
#define PICKUP_FORMAT_H

#include <stddef.h>

/*
 * Turning numbers into something a person reads at a glance.
 */

/* Room for the longest size this produces, terminator included. */
#define FORMAT_SIZE_MAX 16

/* Write `bytes` as a compact size ("512B", "1.8G") into `out`.

   Units go up in steps of 1024 and are named K, M and G, matching what `ls -h`
   and `du -h` report, so a size read here agrees with the same file measured
   with the usual tools. One decimal is kept below 10 and dropped above it,
   where it stops carrying information: "1.8G" is worth saying, "774.3M" is
   not. */
void format_size(long long bytes, char *out, size_t out_size);

/* Marks for a finished and a failed operation.

   A check mark where the console can render one, "ok" and "x" where it cannot:
   a terminal that is not reading UTF-8 would show a run of replacement
   characters, which reads as a bug rather than as success. Decided from the
   locale, which is what says how the output will be interpreted. */
[[nodiscard]] const char *format_check(void);
[[nodiscard]] const char *format_cross(void);

#endif /* PICKUP_FORMAT_H */
