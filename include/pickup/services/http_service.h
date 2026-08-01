#ifndef PICKUP_HTTP_SERVICE_H
#define PICKUP_HTTP_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Fetching things over HTTPS.
 *
 * Pickup delegates this to the system's curl rather than linking a library,
 * which keeps it building against libc alone and works on all three target
 * platforms: curl ships with Windows 10 and later, and with every macOS and
 * mainstream Linux. If it is missing, that is reported plainly instead of
 * being worked around.
 */

/* True if a usable curl was found. The answer is worked out once. */
[[nodiscard]] bool http_available(void);

/* The command a caller should be told to install when http_available is false. */
[[nodiscard]] const char *http_requirement(void);

/* Download `url` into `dest`, replacing whatever was there. False on any
   transport or HTTP error, including 404: a page of error text saved under the
   name of an archive would fail verification later and confuse the report.
   The destination is left absent rather than half-written. */
[[nodiscard]] bool http_download(const char *url, const char *dest);

/* Download `url` into `dest`, drawing a progress bar labelled `label`.

   `expected_size` is what the source said the file would be, and is what the
   bar is measured against; pass 0 when it is unknown and no bar is drawn. The
   bar only appears on a terminal. */
[[nodiscard]] bool http_download_with_progress(const char *url, const char *dest,
                                               long long expected_size,
                                               const char *label);

/* Told, about ten times a second, that the transfer is still running.

   For a download whose size the source never announces — an API answer, rather
   than a release asset — there is no fraction to report, and that it is still
   moving is the whole message. `frame` only ever grows. */
typedef void (*http_tick)(size_t frame, void *context);

/* Download `url` into `dest`, ticking while it runs. A NULL `tick` is the plain
   download. */
[[nodiscard]] bool http_download_watched(const char *url, const char *dest,
                                         http_tick tick, void *context);

#endif /* PICKUP_HTTP_SERVICE_H */
