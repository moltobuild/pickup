#include <pickup/detect/distro.h>

#include <pickup/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The two fields worth reading. ID is what the distribution calls itself,
   ID_LIKE what it descends from: Ubuntu is ID=ubuntu, ID_LIKE=debian. */
#define FIELD_ID "ID"
#define FIELD_ID_LIKE "ID_LIKE"

/* How a family names the package that carries the C++ compiler. Only the name
   is convention; which family is in force was read from the system. */
#define PACKAGE_DEBIAN_FORMAT "g++-%d"
#define PACKAGE_FEDORA "gcc-c++"
#define PACKAGE_SUSE_FORMAT "gcc%d-c++"
#define PACKAGE_ALPINE "g++"

/* Identifiers that map to a family, matched against ID and then ID_LIKE. */
typedef struct {
    const char *id;
    distro_family family;
} distro_alias;

static const distro_alias aliases[] = {
    {"debian", distro_debian},
    {"ubuntu", distro_debian},
    {"linuxmint", distro_debian},
    {"pop", distro_debian},
    {"raspbian", distro_debian},
    {"fedora", distro_fedora},
    {"rhel", distro_fedora},
    {"centos", distro_fedora},
    {"rocky", distro_fedora},
    {"almalinux", distro_fedora},
    {"arch", distro_arch},
    {"archarm", distro_arch},
    {"manjaro", distro_arch},
    {"endeavouros", distro_arch},
    {"opensuse", distro_suse},
    {"opensuse-leap", distro_suse},
    {"opensuse-tumbleweed", distro_suse},
    {"sles", distro_suse},
    {"suse", distro_suse},
    {"alpine", distro_alpine},
};
#define ALIAS_COUNT (sizeof aliases / sizeof aliases[0])

const char *distro_family_name(distro_family family) {
    switch (family) {
    case distro_debian:
        return "debian";
    case distro_fedora:
        return "fedora";
    case distro_arch:
        return "arch";
    case distro_suse:
        return "suse";
    case distro_alpine:
        return "alpine";
    case distro_unknown:
        return "unknown";
    }
    return "unknown";
}

/* Copy the value of `field` out of an os-release file into `out`. Values may
   be quoted, and ID_LIKE may hold several separated by spaces. */
static bool read_field(const char *text, const char *field, char *out, size_t out_size) {
    size_t field_length = strlen(field);

    for (const char *line = text; line != NULL && *line != '\0';) {
        /* Only a match at the start of a line is the field: ID would
           otherwise match inside VERSION_ID. */
        bool at_line_start = line == text || line[-1] == '\n';
        if (at_line_start && strncmp(line, field, field_length) == 0 && line[field_length] == '=') {
            const char *value = line + field_length + 1;
            size_t length = 0;
            while (value[length] != '\0' && value[length] != '\n')
                length++;

            /* Strip the quotes the format allows around a value. */
            if (length >= 2 && (value[0] == '"' || value[0] == '\'') &&
                value[length - 1] == value[0]) {
                value++;
                length -= 2;
            }
            if (length == 0 || length >= out_size)
                return false;
            memcpy(out, value, length);
            out[length] = '\0';
            return true;
        }

        const char *newline = strchr(line, '\n');
        line = newline != NULL ? newline + 1 : NULL;
    }
    return false;
}

/* The family an identifier names, or distro_unknown. */
static distro_family family_of(const char *id) {
    for (size_t i = 0; i < ALIAS_COUNT; i++) {
        if (strcmp(id, aliases[i].id) == 0)
            return aliases[i].family;
    }
    return distro_unknown;
}

/* The first recognised family among the space-separated ids in `list`. */
static distro_family family_of_list(const char *list) {
    const char *cursor = list;
    while (*cursor != '\0') {
        while (*cursor == ' ')
            cursor++;
        const char *space = strchr(cursor, ' ');
        size_t length = space != NULL ? (size_t)(space - cursor) : strlen(cursor);

        if (length > 0 && length < DISTRO_PACKAGE_MAX) {
            char id[DISTRO_PACKAGE_MAX];
            memcpy(id, cursor, length);
            id[length] = '\0';
            distro_family family = family_of(id);
            if (family != distro_unknown)
                return family;
        }

        if (space == NULL)
            break;
        cursor = space + 1;
    }
    return distro_unknown;
}

distro_family distro_parse_os_release(const char *text) {
    if (text == NULL)
        return distro_unknown;

    char value[DISTRO_PACKAGE_MAX];
    if (read_field(text, FIELD_ID, value, sizeof value)) {
        distro_family family = family_of(value);
        if (family != distro_unknown)
            return family;
    }
    /* A derivative Pickup has never heard of still names its parent. */
    if (read_field(text, FIELD_ID_LIKE, value, sizeof value))
        return family_of_list(value);
    return distro_unknown;
}

distro_family distro_detect(void) {
    char *text = fs_read_file(DISTRO_OS_RELEASE_PATH);
    if (text == NULL)
        return distro_unknown;

    distro_family family = distro_parse_os_release(text);
    free(text);
    return family;
}

bool distro_gxx_package(distro_family family, int gcc_major, char *out, size_t out_size) {
    switch (family) {
    case distro_debian:
        /* Versioned, because several GCCs coexist and only one of them is the
           incomplete one. */
        return gcc_major > 0 && fs_format_path(out, out_size, PACKAGE_DEBIAN_FORMAT, gcc_major);
    case distro_suse:
        return gcc_major > 0 && fs_format_path(out, out_size, PACKAGE_SUSE_FORMAT, gcc_major);
    case distro_fedora:
        return fs_format_path(out, out_size, "%s", PACKAGE_FEDORA);
    case distro_alpine:
        return fs_format_path(out, out_size, "%s", PACKAGE_ALPINE);
    /* Arch ships both compilers in one package, so a GCC without C++ is not a
       state that family can be in; an unrecognised distro has no package name
       to offer in the first place. Neither has anything to suggest, and one
       arm says so once. */
    case distro_arch:
    case distro_unknown:
        return false;
    }
    return false;
}
