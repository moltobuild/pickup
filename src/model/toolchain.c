#include <pickup/model/toolchain.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vendor names, indexed by toolchain_vendor. */
static const char *const vendor_names[] = {
    [vendor_unknown]     = "unknown",
    [vendor_gcc]         = "gcc",
    [vendor_clang]       = "clang",
    [vendor_apple_clang] = "apple-clang",
    [vendor_msvc]        = "msvc",
};

#define VENDOR_COUNT (sizeof vendor_names / sizeof vendor_names[0])

const char *toolchain_vendor_name(toolchain_vendor vendor) {
    if ((size_t)vendor >= VENDOR_COUNT)
        return vendor_names[vendor_unknown];
    return vendor_names[vendor];
}

toolchain_vendor toolchain_vendor_parse(const char *name) {
    for (size_t i = 0; i < VENDOR_COUNT; i++) {
        if (strcmp(vendor_names[i], name) == 0)
            return (toolchain_vendor)i;
    }
    return vendor_unknown;
}

void toolchain_version_format(toolchain_version version, char *out, size_t out_size) {
    snprintf(out, out_size, "%d.%d.%d", version.major, version.minor, version.patch);
}

bool toolchain_version_parse(const char *text, toolchain_version *out) {
    char *end = NULL;
    long major = strtol(text, &end, 10);
    if (end == text)
        return false;

    *out = (toolchain_version){ .major = (int)major, .minor = 0, .patch = 0 };
    if (*end != '.')
        return true;

    const char *cursor = end + 1;
    out->minor = (int)strtol(cursor, &end, 10);
    if (*end != '.')
        return true;

    cursor = end + 1;
    out->patch = (int)strtol(cursor, &end, 10);
    return true;
}

int toolchain_version_compare(toolchain_version a, toolchain_version b) {
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;
    return 0;
}
