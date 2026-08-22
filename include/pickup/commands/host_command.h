#ifndef PICKUP_HOST_COMMAND_H
#define PICKUP_HOST_COMMAND_H

#include <stdbool.h>

/*
 * `pickup host` — what the registry calls this machine.
 *
 * There is one vocabulary of target names in this ecosystem and it belongs
 * here: pickup is what downloads published artifacts, so pickup is what has to
 * agree with the registry about which of them could run. Every other tool that
 * needs the answer asks rather than deriving one, because a second derivation
 * disagrees with this one the first time an architecture is spelled
 * differently.
 *
 * Note this is *not* the compiler's target triple. `pickup resolve` reports
 * `x86_64-unknown-linux-gnu`, which is what a driver was built to emit code
 * for; this reports `linux-x86_64`, which is what the catalogue publishes
 * under. They answer different questions and are deliberately different
 * strings.
 *
 * Exits 3 — nothing matched — on a host the registry publishes nothing for,
 * which is an answer and not a failure.
 */
[[nodiscard]] int host_command_run(bool as_toml);

#endif /* PICKUP_HOST_COMMAND_H */
