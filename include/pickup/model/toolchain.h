#ifndef PICKUP_TOOLCHAIN_H
#define PICKUP_TOOLCHAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pickup/detect/capability.h>

/* Buffer sizes for the fields Pickup records about a toolchain. */
#define PICKUP_PATH_MAX    4096
#define PICKUP_TARGET_MAX  128
#define PICKUP_NAME_MAX    64

/* Room for the canonical identity, "gcc@12.3.0-conda". */
#define PICKUP_ID_MAX      160

/* Where a toolchain came from.

   The distinction that matters to someone reading a list: one of these Pickup
   installed and can remove again, the rest were already on the machine and are
   the package manager's business, not Pickup's. */
typedef enum {
    toolchain_source_system,  /* found on PATH; Pickup did not put it there */
    toolchain_source_pickup,  /* installed by Pickup, under its own home */
} toolchain_source;

/* Who made the compiler. Determined by asking it, never by its filename:
   `gcc` on macOS is Clang. */
typedef enum {
    vendor_unknown,
    vendor_gcc,
    vendor_clang,
    vendor_apple_clang,
    vendor_msvc,
} toolchain_vendor;

typedef struct {
    int major;
    int minor;
    int patch;
} toolchain_version;

/* A compiler Pickup can invoke, and everything it learned about it. */
typedef struct {
    char path[PICKUP_PATH_MAX];      /* the real binary, symlinks resolved */
    char name[PICKUP_NAME_MAX];      /* basename of the binary, as invoked */
    char id[PICKUP_ID_MAX];          /* canonical identity, "gcc@12.3.0" */
    char cxx_path[PICKUP_PATH_MAX];  /* companion C++ driver; "" if none found */
    toolchain_vendor vendor;
    toolchain_version version;
    char target[PICKUP_TARGET_MAX];  /* target triple, from -dumpmachine */
    toolchain_source source;
    capability_set c_features;       /* what it actually compiles */
    capability_set cxx_features;
} toolchain;

/*
 * The canonical identity: what the toolchain is, rather than what a link to it
 * happens to be called.
 *
 * A single GCC answers to `cc`, `gcc-9`, `c89-gcc` and `g++-9`, and listing
 * those as four entries says four things where there is one. The name that
 * identifies it is its vendor and version — `gcc@9.5.0` — in the shape package
 * managers already use for exactly this.
 *
 * A target that is not the plain one for this host adds a suffix, so a GCC
 * built for `x86_64-conda-linux-gnu` is `gcc@12.3.0-conda` and does not
 * collide with the system's own 12.3.0. The suffix comes from the target
 * itself, never from comparing toolchains with each other: a name that changed
 * because something unrelated was installed would be no name at all.
 */
void toolchain_make_id(toolchain *chain);

/* The distinguishing part of a target triple, or "" when it is the ordinary
   one: "x86_64-conda-linux-gnu" yields "conda", "x86_64-linux-gnu" yields
   nothing. */
void toolchain_target_tag(const char *target, char *out, size_t out_size);

/* Human-readable source name ("system", "pickup"). Never NULL. */
[[nodiscard]] const char *toolchain_source_name(toolchain_source source);

/*
 * True if `query` names this toolchain.
 *
 * Accepts the full identity, and the shorter forms a person types:
 *
 *   gcc@12.3.0-conda   exactly this one
 *   gcc@12.3.0         the version, whatever its target tag
 *   gcc@12             any 12
 *   gcc@latest, gcc    any of them, left to the caller to rank
 *   /usr/bin/gcc-12    the path, or the binary's own name
 */
[[nodiscard]] bool toolchain_matches(const toolchain *chain, const char *query);

/* Human-readable vendor name ("gcc", "clang", ...). Never NULL. */
[[nodiscard]] const char *toolchain_vendor_name(toolchain_vendor vendor);

/* Parse a vendor name back into the enum; vendor_unknown if unrecognized. */
[[nodiscard]] toolchain_vendor toolchain_vendor_parse(const char *name);

/* Format a version as "12.3.0" into `out`. */
void toolchain_version_format(toolchain_version version, char *out, size_t out_size);

/* Parse "12.3.0", "12.3" or "12" into a version. Missing components are 0.
   Returns false if there is no leading number at all. */
[[nodiscard]] bool toolchain_version_parse(const char *text, toolchain_version *out);

/* Order two versions: negative, zero or positive, like strcmp. */
[[nodiscard]] int toolchain_version_compare(toolchain_version a, toolchain_version b);

#endif /* PICKUP_TOOLCHAIN_H */
