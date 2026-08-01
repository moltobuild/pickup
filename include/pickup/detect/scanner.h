#ifndef PICKUP_SCANNER_H
#define PICKUP_SCANNER_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/util/str_list.h>

/* Collect the compiler candidates found in the directories of `path_env` (the
   value of PATH), appending each one's resolved absolute path to `out`.

   A name only makes something a candidate; it never decides what it is. Each
   candidate is deduplicated by the file it resolves to, so `cc`, `gcc` and
   `gcc-9` pointing at one binary are reported once.

   Returns false only on allocation failure: an unreadable directory in PATH is
   skipped, not fatal. */
[[nodiscard]] bool scanner_collect(const char *path_env, str_list *out);

/* Add the candidates from toolchains Pickup installed itself, found under
   `toolchains_dir` as <toolchain>/bin.

   `out` may already hold what PATH turned up, and is deduplicated against it,
   so a toolchain that is both installed here and reachable on PATH is still
   reported once. A missing directory simply means nothing has been installed,
   which is not a failure. */
[[nodiscard]] bool scanner_collect_installed(const char *toolchains_dir, str_list *out);

/* True if `name` looks like a compiler worth probing. Exposed for testing. */
[[nodiscard]] bool scanner_is_candidate_name(const char *name);

#endif /* PICKUP_SCANNER_H */
