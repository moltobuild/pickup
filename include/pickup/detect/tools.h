#ifndef PICKUP_TOOLS_H
#define PICKUP_TOOLS_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/model/toolchain.h>
#include <pickup/services/paths_service.h>

/*
 * The tools a project needs besides a compiler.
 *
 * A machine that compiles is not a machine that can be worked on: without a
 * formatter every diff argues about whitespace, and without a linter the
 * mistakes a compiler is not asked to catch go unnoticed. Neither is exotic,
 * both ship with LLVM, and a report on the state of a build environment that
 * says nothing about them is answering only half the question.
 *
 * Found the way compilers are: by name, and then by asking. A `clang-format`
 * that does not answer `--version` is a name on a filesystem, not a tool, and
 * a report claiming otherwise would be the kind of unproven claim this project
 * exists to refuse.
 */

typedef enum {
    tool_formatter,
    tool_linter,
} tool_kind;

#define TOOL_NAME_MAX 64
/* What a tool prints when asked. Generous because these are sentences, not
   version numbers: clang-format answers with its version, the URL of the
   feedstock it was built from, and a commit hash. */
#define TOOL_VERSION_MAX 256

/* Most tools reported. Two kinds, with a few candidates each. */
#define TOOLS_MAX 8

typedef struct {
    tool_kind kind;
    char name[TOOL_NAME_MAX];       /* "clang-format" */
    char path[PICKUP_PATHS_MAX];    /* where it was found */
    char version[TOOL_VERSION_MAX]; /* what it said when asked */
    /* Who is responsible for it, decided by where it lives — the same
       distinction `list` draws for compilers. */
    toolchain_source source;
} dev_tool;

/* The tools this machine has, searched on PATH and in the toolchains Pickup
   installed. Returns how many were written to `out`. */
[[nodiscard]] size_t tools_discover(dev_tool *out, size_t max);

/* The name of a kind, for reports: "formatter", "linter". Never NULL. */
[[nodiscard]] const char *tool_kind_name(tool_kind kind);

/* What Pickup would install to provide `kind`, as a package name it knows how
   to fetch. Never NULL. */
[[nodiscard]] const char *tool_kind_package(tool_kind kind);

/* True if `name` is a tool Pickup knows how to look for, filling `kind`. Used
   by `install` to tell a tool name from a toolchain name. */
[[nodiscard]] bool tools_kind_of(const char *name, tool_kind *kind);

#endif /* PICKUP_TOOLS_H */
