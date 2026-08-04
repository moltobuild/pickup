#ifndef PICKUP_TOOLS_COMMAND_H
#define PICKUP_TOOLS_COMMAND_H

#include <stdbool.h>

/*
 * Execute `pickup tools`: the formatter and the linter this machine has, and
 * where they are.
 *
 * A catalogue, not a diagnosis. `doctor` says whether one is missing, which is
 * a problem to act on; this says which ones are there and what to invoke,
 * which is what a build needs in order to use them. Keeping the two apart is
 * why `list` exists beside `doctor` for compilers, and the reason is the same:
 * a caller reading paths out of a report of problems is reading the wrong
 * document.
 *
 * Always exit_ok. A machine with no tools is a fact about the machine, not a
 * failure of the question.
 */
[[nodiscard]] int tools_command_run(bool as_toml);

#endif /* PICKUP_TOOLS_COMMAND_H */
