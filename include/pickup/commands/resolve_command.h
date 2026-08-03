#ifndef PICKUP_RESOLVE_COMMAND_H
#define PICKUP_RESOLVE_COMMAND_H

#include <stdbool.h>

/* What a caller needs from a toolchain. Every field is optional except the
   language: asking for nothing valid returns the best compiler available. */
typedef struct {
    const char *lang;      /* "c" or "c++"; NULL means C */
    const char *standard;  /* e.g. "c2x"; NULL means no standard requirement */
    const char *features;  /* comma-separated feature ids; NULL means none */
    const char *vendor;    /* restrict to one vendor; NULL means any */
    /* "libstdc++" or "libc++"; NULL takes whichever the toolchain works with.

       Only worth naming when the project has an ABI constraint Pickup cannot
       see — linking against a prebuilt C++ library commits everything else to
       the same standard library, and the two do not mix. */
    const char *stdlib;
} resolve_request;

/* Execute `pickup resolve`: print the best toolchain satisfying `request`.
   Returns exit_ok, exit_no_match when nothing qualifies, or exit_usage_error
   when the request itself is malformed. */
[[nodiscard]] int resolve_command_run(const resolve_request *request, bool as_toml);

#endif /* PICKUP_RESOLVE_COMMAND_H */
