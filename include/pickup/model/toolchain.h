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
    char name[PICKUP_NAME_MAX];      /* basename, for display */
    char cxx_path[PICKUP_PATH_MAX];  /* companion C++ driver; "" if none found */
    toolchain_vendor vendor;
    toolchain_version version;
    char target[PICKUP_TARGET_MAX];  /* target triple, from -dumpmachine */
    capability_set c_features;       /* what it actually compiles */
    capability_set cxx_features;
} toolchain;

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
