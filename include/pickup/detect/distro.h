#ifndef PICKUP_DISTRO_H
#define PICKUP_DISTRO_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Which distribution this is, and what it calls things.
 *
 * Pickup is not the operating system's package manager (spec.md section 3) and
 * will not become one. But a diagnosis that stops at "this GCC has no C++
 * support" leaves the reader to work out what to install, and the name of that
 * package is the one piece of the answer they cannot derive themselves.
 *
 * So the family is read out of /etc/os-release, which is the system stating
 * what it is, and only the package name comes from convention. Where the
 * family is not recognised the line is left out rather than invented: a
 * suggestion to install something that does not exist is worse than no
 * suggestion.
 *
 * Nothing here runs a package manager. Naming a package is not managing one.
 */

typedef enum {
    distro_unknown,
    distro_debian,   /* Debian, Ubuntu, Mint, Pop!_OS */
    distro_fedora,   /* Fedora, RHEL, CentOS, Rocky */
    distro_arch,
    distro_suse,
    distro_alpine,
} distro_family;

/* Where the system describes itself. */
#define DISTRO_OS_RELEASE_PATH "/etc/os-release"

/* Longest package name reported. */
#define DISTRO_PACKAGE_MAX 64

/* Read the family from /etc/os-release. distro_unknown when the file is
   absent or names nothing recognised, which is an answer. */
[[nodiscard]] distro_family distro_detect(void);

/* The same from the contents of such a file, so the mapping can be tested
   against recorded ones rather than only against the machine running the
   tests. Both `ID` and `ID_LIKE` are consulted: a derivative names its parent
   in the second when the first is its own. */
[[nodiscard]] distro_family distro_parse_os_release(const char *text);

/* The package that adds C++ support to a GCC of major version `gcc_major`.

   False when the family is unknown, or when there is nothing to name: on Arch
   the C++ compiler is part of the same package as the C one, so there is no
   second thing to install and pretending otherwise would send the reader
   looking for it. */
[[nodiscard]] bool distro_gxx_package(distro_family family, int gcc_major,
                                      char *out, size_t out_size);

/* The family's name, for reports. Never NULL. */
[[nodiscard]] const char *distro_family_name(distro_family family);

#endif /* PICKUP_DISTRO_H */
