#ifndef PICKUP_CONDA_SOURCE_H
#define PICKUP_CONDA_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/services/http_service.h>
#include <pickup/util/sha256.h>

/*
 * What conda-forge publishes, read straight from the channel.
 *
 * conda-forge is where compilers and libraries for them come from when the
 * system has none worth using, and it is reached without conda: there is no
 * client to install, no environment to activate, and no Python. The channel
 * answers JSON over HTTPS, which Pickup already parses, and ships packages as
 * zip files, which it can already open.
 *
 * Two endpoints, chosen for size:
 *
 *   /package/conda-forge/<name>    every build of one package, about a
 *                                  megabyte, with the dependencies of each
 *   /dist/conda-forge/<name>/…     one build, under a kilobyte, and the only
 *                                  place the sha256 appears
 *
 * The channel's own repodata.json holds all of it in one file and is over four
 * hundred megabytes, which is why it is not used. The per-package listing
 * publishes md5 and not sha256; the per-build one publishes sha256, so the
 * digest is fetched for the handful of packages actually being installed. That
 * keeps the rule that nothing is installed without a digest to check it
 * against (spec.md section 11).
 */

#define CONDA_NAME_MAX    128
#define CONDA_VERSION_MAX 64
#define CONDA_BUILD_MAX   64
#define CONDA_FILE_MAX    256
#define CONDA_URL_MAX     1024

/* Most dependencies one package declares. The compilers sit around eight. */
#define CONDA_MAX_DEPENDS 32

/* A dependency as the channel states it: a name, and optionally a version
   constraint and a build string. */
#define CONDA_SPEC_MAX 160

/* Longest channel subdirectory name. */
#define CONDA_SUBDIR_MAX 32

/* One installable build of one package. */
typedef struct {
    char name[CONDA_NAME_MAX];
    char version[CONDA_VERSION_MAX];
    char build[CONDA_BUILD_MAX];
    char subdir[CONDA_SUBDIR_MAX];    /* the host's, or "noarch" */
    char file[CONDA_FILE_MAX];        /* "gcc_impl_linux-64-16.1.0-h5.conda" */
    char url[CONDA_URL_MAX];
    char sha256[SHA256_HEX_SIZE];     /* "" until conda_fetch_sha256 fills it */
    long long size;
    long long timestamp;              /* when it was built; newest build wins */
    char depends[CONDA_MAX_DEPENDS][CONDA_SPEC_MAX];
    size_t depend_count;
} conda_package;

typedef struct {
    conda_package *items;
    size_t count;
    size_t capacity;
} conda_list;

void conda_list_init(conda_list *list);
void conda_list_free(conda_list *list);
[[nodiscard]] bool conda_list_push(conda_list *list, const conda_package *package);

/* The channel subdirectory this host installs from: "linux-64", "osx-arm64".
   Never NULL; an unsupported host yields "noarch", which carries no compiler
   and so resolves to nothing rather than to something wrong. */
[[nodiscard]] const char *conda_subdir(void);

/* The architecture-independent subdirectory, which every host also installs
   from.

   It is not only for scripts and data. conda-forge publishes the development
   halves of its compilers there — `libstdcxx-devel_linux-64` is headers and
   archives with nothing to execute — so a resolver that read only the host's
   own subdirectory would find the compiler and none of what it compiles
   against, which is a toolchain that cannot build. */
#define CONDA_NOARCH "noarch"

/* Read the builds of one package out of an API answer, keeping those for this
   host that match `version_filter` (NULL or "" for all). Newest build first. */
[[nodiscard]] bool conda_parse_packages(const char *json, const char *version_filter,
                                        conda_list *out);

/* The same, fetched from the channel and cached briefly. */
[[nodiscard]] bool conda_fetch_packages(const char *name, const char *version_filter,
                                        bool refresh, conda_list *out);

/* The same, ticking while the listing is on its way.

   Only the fetch is watched, because only the fetch takes time worth
   reporting: a cached listing parses in an instant and ticks not at all. */
[[nodiscard]] bool conda_fetch_packages_watched(const char *name,
                                                const char *version_filter,
                                                bool refresh, http_tick tick,
                                                void *context, conda_list *out);

/* Fill in `package->sha256` from the channel. False when the channel publishes
   none, which is what stops an unverifiable package from being installed. */
[[nodiscard]] bool conda_fetch_sha256(conda_package *package);

/* The newest build in `list`, or false when it is empty. */
[[nodiscard]] bool conda_best(const conda_list *list, conda_package *out);

/*
 * Dependency specifications, in the reduced form the toolchain packages use:
 *
 *   sysroot_linux-64                      any build
 *   libgcc >=16.1.0                       at least this version
 *   gcc_impl_linux-64 15.3.0.*            this version, any patch, any build
 *   libsanitizer 16.1.0 hf2715c6_1        this version and this build exactly
 *   binutils >=2.46,<3.0.a0               a half-open range
 *
 * Anything richer than that is not understood and is reported as such rather
 * than approximated: an approximated constraint installs the wrong thing, and
 * doing so quietly is worse than refusing.
 */
typedef struct {
    char name[CONDA_NAME_MAX];
    char version[CONDA_VERSION_MAX];      /* "" when unconstrained */
    char version_below[CONDA_VERSION_MAX];/* exclusive upper bound; "" if none */
    char build[CONDA_BUILD_MAX];          /* "" when unconstrained */
    bool at_least;                        /* the constraint was ">=" */
} conda_spec;

/*
 * True if `name` is a virtual package.
 *
 * conda states some requirements about the machine as though they were
 * packages — `__glibc >=2.17`, `__unix`, `__cuda` — and those are never
 * downloaded, because there is nothing to download: they describe what the
 * system already provides. A resolver that treated one as missing would refuse
 * to install a toolchain over a requirement that was already met.
 */
[[nodiscard]] bool conda_is_virtual(const char *name);

/* Split a specification into its parts. False when it is not a form Pickup
   understands. */
[[nodiscard]] bool conda_spec_parse(const char *text, conda_spec *out);

/* True if `package` satisfies `spec`. */
[[nodiscard]] bool conda_spec_matches(const conda_spec *spec, const conda_package *package);

#endif /* PICKUP_CONDA_SOURCE_H */
