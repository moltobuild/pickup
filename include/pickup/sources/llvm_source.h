#ifndef PICKUP_LLVM_SOURCE_H
#define PICKUP_LLVM_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/util/sha256.h>

/*
 * What the LLVM project publishes, and which of it runs on this machine.
 *
 * Releases are read from GitHub's REST API, which answers JSON: no page is
 * ever scraped. Two things here need care and are therefore kept apart from
 * the network, so they can be tested against a recorded answer:
 *
 *   - picking the asset for this host, because one release carries source
 *     archives, documentation, installers and several platforms at once, and
 *     the naming has changed across versions
 *   - matching a partial version, so "14" and "14.2" mean what a person means
 *
 * Release candidates are never offered. A prerelease is flagged as such by the
 * API, and installing one by accident is not something a version number should
 * be able to cause.
 */

#define RELEASE_VERSION_MAX 32
#define RELEASE_TAG_MAX     64
#define RELEASE_ASSET_MAX   256
#define RELEASE_URL_MAX     1024

/* One installable archive. */
typedef struct {
    char version[RELEASE_VERSION_MAX];  /* "22.1.8" */
    char tag[RELEASE_TAG_MAX];          /* "llvmorg-22.1.8" */
    char asset[RELEASE_ASSET_MAX];      /* the file name as published */
    char url[RELEASE_URL_MAX];
    char sha256[SHA256_HEX_SIZE];       /* "" when the source published none */
    long long size;                     /* bytes, 0 if unknown */
} release_asset;

typedef struct {
    release_asset *items;
    size_t count;
    size_t capacity;
} release_list;

void release_list_init(release_list *list);
void release_list_free(release_list *list);

/* Read releases out of an API answer, keeping the stable ones that ship an
   asset for this host and match `version_filter` (NULL or "" for all).
   Newest version first. */
[[nodiscard]] bool llvm_parse_releases(const char *json, const char *version_filter,
                                       release_list *out);

/* The same, fetched from GitHub. The answer is cached briefly, because the
   unauthenticated API allows only 60 requests an hour and `search` is the kind
   of command people run twice in a row. */
[[nodiscard]] bool llvm_fetch_releases(const char *version_filter, release_list *out);

/* The highest version in `list`. False if it is empty. */
[[nodiscard]] bool llvm_best(const release_list *list, release_asset *out);

/* True if `name` is the archive this host can run, as opposed to source,
   documentation, an installer, a signature, or another platform's build. */
[[nodiscard]] bool llvm_asset_matches_host(const char *name);

/* True if `version` satisfies `filter`, which may name fewer components than
   the version has: "14" matches 14.2.1, "14.2" does not match 14.3.0. NULL or
   "" matches everything. */
[[nodiscard]] bool llvm_version_matches(const char *filter, const char *version);

#endif /* PICKUP_LLVM_SOURCE_H */
