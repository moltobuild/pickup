#ifndef PICKUP_CONDA_CLOSURE_H
#define PICKUP_CONDA_CLOSURE_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/sources/conda_source.h>

/*
 * Everything one toolchain needs, gathered.
 *
 * A compiler on conda-forge is not one package. `gcc_impl_linux-64` is 85 MB
 * and names eight more: the assembler, the sysroot, libgcc, libstdc++ and the
 * development halves of both. Install the first alone and nothing compiles.
 *
 * This is emphatically not a dependency solver, and Pickup does not become a
 * package manager by having it (spec.md section 3). There is no backtracking
 * and no search: the graph of a toolchain is small and its constraints are
 * already consistent, because the channel built those packages against each
 * other. So it is a walk — take the newest build satisfying each requirement,
 * follow what it names, stop when nothing new appears.
 *
 * When a requirement cannot be met, the walk stops and says which one. That is
 * the same answer `resolve` gives a request it cannot satisfy: naming what is
 * missing is part of the answer, and installing most of a toolchain would be
 * worse than installing none of it.
 */

/* Most packages one toolchain pulls in. The compilers reach about a dozen. */
#define CONDA_MAX_CLOSURE 64

typedef struct {
    conda_list packages;              /* every package to install, root first */
    bool complete;                    /* false when something could not be met */
    char missing[CONDA_SPEC_MAX];     /* the requirement that stopped it */
    char required_by[CONDA_NAME_MAX]; /* and who asked for it */
} conda_closure;

void conda_closure_init(conda_closure *closure);
void conda_closure_free(conda_closure *closure);

/* Gather everything `root` needs, at `version_filter` (NULL for the newest).

   Returns false only when the channel could not be reached or read. A closure
   that could not be completed is a result: `complete` is false and `missing`
   names the requirement, which is something a caller can report. */
[[nodiscard]] bool conda_resolve(const char *root, const char *version_filter,
                                 bool refresh, conda_closure *out);

/* The total size of a closure, in bytes. */
[[nodiscard]] long long conda_closure_size(const conda_closure *closure);

/* Told which package the walk is about to look up, and how many it has settled
   so far.

   Gathering a closure is one request per package — a dozen or more for a
   compiler — and every one of them is a round trip to the channel. Silence for
   that long is indistinguishable from a command that has hung, and unlike a
   download there is no transfer to measure: what there is to report is which
   package is being asked about, which is also the more useful thing to say. */
typedef void (*conda_watch)(const char *name, size_t resolved, void *context);

/* `conda_resolve`, reporting as it goes. A NULL `watch` is the quiet one. */
[[nodiscard]] bool conda_resolve_watched(const char *root, const char *version_filter,
                                         bool refresh, conda_watch watch,
                                         void *context, conda_closure *out);

/*
 * Resolve against packages already in hand rather than the channel.
 *
 * The walk is the part worth testing — which build is chosen, what happens
 * when a constraint cannot be met — and testing it over the network would
 * make the result depend on what conda-forge published this morning.
 *
 * `available` is every build of every package the walk may draw on.
 */
[[nodiscard]] bool conda_resolve_from(const conda_list *available, const char *root,
                                      const char *version_filter, conda_closure *out);

#endif /* PICKUP_CONDA_CLOSURE_H */
