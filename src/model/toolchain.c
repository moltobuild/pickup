#include <pickup/model/toolchain.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vendor names, indexed by toolchain_vendor. */
static const char *const vendor_names[] = {
    [vendor_unknown] = "unknown",         [vendor_gcc] = "gcc",   [vendor_clang] = "clang",
    [vendor_apple_clang] = "apple-clang", [vendor_msvc] = "msvc",
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

    *out = (toolchain_version){.major = (int)major, .minor = 0, .patch = 0};
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

static const char *const source_names[] = {
    [toolchain_source_system] = "system",
    [toolchain_source_pickup] = "pickup",
};

#define SOURCE_COUNT (sizeof source_names / sizeof source_names[0])

const char *toolchain_source_name(toolchain_source source) {
    if ((size_t)source >= SOURCE_COUNT)
        return source_names[toolchain_source_system];
    return source_names[source];
}

/*
 * Parts of a target triple that say nothing about which toolchain this is.
 *
 * Every triple carries an architecture, an operating system and an ABI, and
 * most carry a vendor field that is filled in with a placeholder. None of that
 * distinguishes two compilers on the same machine, so none of it belongs in a
 * name. What is left over does: `conda` marks a toolchain built for a conda
 * prefix, `musl` one built against a different C library.
 */
static const char *const ordinary_target_parts[] = {
    /* architectures */
    "x86_64",
    "i386",
    "i486",
    "i586",
    "i686",
    "aarch64",
    "arm64",
    "armv7l",
    "armv6l",
    "riscv64",
    "ppc64le",
    "ppc64",
    "s390x",
    "mips64",
    /* operating systems and ABIs */
    "linux",
    "gnu",
    "gnueabi",
    "gnueabihf",
    "darwin",
    "windows",
    "elf",
    "eabi",
    "eabihf",
    "msvc",
    /* vendor fields that name nobody */
    "unknown",
    "pc",
    "none",
};

#define ORDINARY_COUNT (sizeof ordinary_target_parts / sizeof ordinary_target_parts[0])

/* True if `part` is one of the components every triple has. Compared over a
   length because the caller hands slices of the triple, not strings. */
static bool is_ordinary_part(const char *part, size_t length) {
    for (size_t i = 0; i < ORDINARY_COUNT; i++) {
        if (strlen(ordinary_target_parts[i]) == length &&
            strncmp(ordinary_target_parts[i], part, length) == 0)
            return true;
    }
    return false;
}

void toolchain_target_tag(const char *target, char *out, size_t out_size) {
    out[0] = '\0';
    if (target == NULL || out_size == 0)
        return;

    /* The first component that is not part of every triple. One is enough:
       triples carry at most one such field, and a name is for reading. */
    const char *cursor = target;
    while (*cursor != '\0') {
        const char *dash = strchr(cursor, '-');
        size_t length = dash != NULL ? (size_t)(dash - cursor) : strlen(cursor);

        if (length > 0 && length < out_size && !is_ordinary_part(cursor, length)) {
            memcpy(out, cursor, length);
            out[length] = '\0';
            return;
        }
        if (dash == NULL)
            break;
        cursor = dash + 1;
    }
}

/* A target tag is one component of a triple, and those are short words:
   "conda", "musl", "apple". Bounded so the identity stays readable. */
#define TARGET_TAG_MAX 32

void toolchain_make_id(toolchain *chain) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);

    char tag[TARGET_TAG_MAX];
    toolchain_target_tag(chain->target, tag, sizeof tag);

    if (tag[0] != '\0')
        snprintf(chain->id, sizeof chain->id, "%s@%s-%s", toolchain_vendor_name(chain->vendor),
                 version, tag);
    else
        snprintf(chain->id, sizeof chain->id, "%s@%s", toolchain_vendor_name(chain->vendor),
                 version);
}

/* What a query asks for: a vendor, and how much of a version. */
#define QUERY_LATEST "latest"

bool toolchain_matches(const toolchain *chain, const char *query) {
    if (query == NULL || query[0] == '\0')
        return false;

    /* The full identity, the path, and the binary's own name: each is
       something a person may reasonably have in front of them. */
    if (strcmp(chain->id, query) == 0 || strcmp(chain->path, query) == 0 ||
        strcmp(chain->name, query) == 0)
        return true;

    const char *at = strchr(query, '@');
    if (at == NULL) {
        /* A bare vendor: "gcc" means any gcc. */
        return strcmp(query, toolchain_vendor_name(chain->vendor)) == 0;
    }

    size_t vendor_length = (size_t)(at - query);
    const char *vendor = toolchain_vendor_name(chain->vendor);
    if (strlen(vendor) != vendor_length || strncmp(query, vendor, vendor_length) != 0)
        return false;

    const char *wanted = at + 1;
    if (wanted[0] == '\0' || strcmp(wanted, QUERY_LATEST) == 0)
        return true;

    /* A version, possibly partial and possibly carrying a target tag. Only the
       components actually named are compared, so "gcc@12" means any 12. */
    char requested[PICKUP_ID_MAX];
    snprintf(requested, sizeof requested, "%s", wanted);
    char *dash = strchr(requested, '-');
    if (dash != NULL) {
        char tag[TARGET_TAG_MAX];
        toolchain_target_tag(chain->target, tag, sizeof tag);
        if (strcmp(dash + 1, tag) != 0)
            return false;
        *dash = '\0';
    }

    toolchain_version version;
    if (!toolchain_version_parse(requested, &version))
        return false;

    int named = 1;
    for (const char *c = requested; *c != '\0'; c++) {
        if (*c == '.')
            named++;
    }
    if (version.major != chain->version.major)
        return false;
    if (named >= 2 && version.minor != chain->version.minor)
        return false;
    if (named >= 3 && version.patch != chain->version.patch)
        return false;
    return true;
}
