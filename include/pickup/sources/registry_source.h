#ifndef PICKUP_REGISTRY_SOURCE_H
#define PICKUP_REGISTRY_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/services/http_service.h>
#include <pickup/util/sha256.h>

/*
 * What the registry publishes, and which of it runs on this machine.
 *
 * One HTTP source answers for everything Pickup installs. It publishes
 * artifacts already trimmed to what a build uses, names its own coordinates —
 * kind, name, version, target — and states the digest and the packing of every
 * blob, so nothing about a download has to be inferred from its URL.
 *
 * Three things need care and are therefore kept apart from the network, so they
 * can be tested against a recorded answer:
 *
 *   - matching a partial version, so "19" and "19.1" mean what a person means
 *   - choosing between the versions of one name, including what to do with one
 *     that has been withdrawn
 *   - naming this host the way the registry names it
 *
 * What the registry says *about* an artifact — the features it proved, the
 * flags it was built with — is carried here so a person can read it before
 * installing. It is never used to configure anything. Those metadata describe
 * the machine that built the artifact, and the flags that make a compiler work
 * name directories on the machine that installs it.
 */

#define REGISTRY_NAME_MAX 64
#define REGISTRY_VERSION_MAX 64
#define REGISTRY_TARGET_MAX 64
#define REGISTRY_FORMAT_MAX 32
#define REGISTRY_URL_MAX 1024
#define REGISTRY_TIME_MAX 32 /* "2026-08-05T19:41:45Z" */
#define REGISTRY_ABOUT_MAX 256
#define REGISTRY_BINARY_MAX 128 /* "bin/clang-format", relative to the prefix */
#define REGISTRY_TARGETS_MAX 8
#define REGISTRY_FEATURES_MAX 40
#define REGISTRY_FEATURE_MAX 32
#define REGISTRY_EMAIL_MAX 255 /* 254 is the longest address, plus the NUL */

/* The one packing this build knows how to open. Published by the registry as
   `format`, and compared rather than guessed. */
#define REGISTRY_FORMAT_TAR_ZST "tar.zst"

/* Where artifacts come from, in falling order of precedence: the environment,
   the configuration file, this. */
#define REGISTRY_URL_ENV "PICKUP_REGISTRY_URL"
#define REGISTRY_DEFAULT_URL "https://molto-registry.molto-build.workers.dev"

typedef enum {
    registry_kind_toolchain,
    registry_kind_tool,
    registry_kind_package,
} registry_kind;

/*
 * One downloadable blob.
 *
 * Every field the registry marks required is required here too: an artifact
 * missing its digest, its size or its URL is a broken answer, and it is dropped
 * while parsing rather than carried to the point of being installed.
 */
typedef struct {
    registry_kind kind;
    char name[REGISTRY_NAME_MAX];
    char version[REGISTRY_VERSION_MAX];
    char target[REGISTRY_TARGET_MAX];
    char format[REGISTRY_FORMAT_MAX];
    char checksum[SHA256_HEX_SIZE]; /* lowercase hex sha256 */
    long long size_bytes;
    bool yanked; /* resolvable, but not for new builds */
    char published_at[REGISTRY_TIME_MAX];
    char download_url[REGISTRY_URL_MAX];

    /* What the registry says about it. Shown, never obeyed. */
    char description[REGISTRY_ABOUT_MAX];
    char vendor[REGISTRY_NAME_MAX];
    char triple[REGISTRY_NAME_MAX];
    char binary[REGISTRY_BINARY_MAX];
    /* Where the C driver sits inside the prefix, when the recipe named it.
       Obeyed, unlike the rest of this block: it is a fact about the archive's
       own layout, not a claim about the machine installing it -- the same
       reason `binary` above is obeyed for a tool. Verified either way, because
       nothing is adopted for having unpacked. */
    char c_driver[REGISTRY_BINARY_MAX];
    char provides[REGISTRY_FEATURES_MAX][REGISTRY_FEATURE_MAX];
    size_t provides_count;

    /* Who published it. Empty for everything published before the registry had
       accounts, which is a fact about the artifact rather than a fault in the
       answer, so it never stops a document from being read. */
    char published_by[REGISTRY_EMAIL_MAX];
} registry_artifact;

typedef struct {
    registry_artifact *items;
    size_t count;
    size_t capacity;
} registry_artifact_list;

/* One line of a catalogue: what exists under a name, without the detail needed
   to install it. */
typedef struct {
    registry_kind kind;
    char name[REGISTRY_NAME_MAX];
    char latest_version[REGISTRY_VERSION_MAX];
    size_t versions;
    char targets[REGISTRY_TARGETS_MAX][REGISTRY_TARGET_MAX];
    size_t target_count;
    char published_at[REGISTRY_TIME_MAX];

    /* Who published the latest version. Shown, never obeyed, and empty when
       that version predates accounts. */
    char published_by[REGISTRY_EMAIL_MAX];
} registry_entry;

typedef struct {
    registry_entry *items;
    size_t count;
    size_t capacity;
} registry_entry_list;

void registry_artifact_list_init(registry_artifact_list *list);
void registry_artifact_list_free(registry_artifact_list *list);
void registry_entry_list_init(registry_entry_list *list);
void registry_entry_list_free(registry_entry_list *list);

/* Append to a list. Public because more than one catalogue is shown as one
   table, and joining them is the caller's business rather than the parser's. */
[[nodiscard]] bool registry_entry_list_push(registry_entry_list *list, const registry_entry *item);

/* --- reading what the registry says --- */

/* A catalogue index: every name published under one kind. */
[[nodiscard]] bool registry_parse_catalogue(const char *json, registry_entry_list *out);

/* A search answer. A row says what exists, not what can be installed, so it
   carries a count of targets rather than their names. */
[[nodiscard]] bool registry_parse_search(const char *json, registry_entry_list *out);

/* Every release of one name, in the order published — most recent first, which
   is the order the registry states and this does not second-guess.

   `version_filter` NULL or "" accepts every version; `target` NULL accepts
   every target. An empty result is an answer, not a failure. */
[[nodiscard]] bool registry_parse_releases(const char *json, const char *version_filter,
                                           const char *target, registry_artifact_list *out);

/* One artifact, as the coordinate endpoint returns it. */
[[nodiscard]] bool registry_parse_artifact(const char *json, registry_artifact *out);

/* The error an unhappy answer carries. `code` is stable and safe to branch on;
   `message` is for a person. */
[[nodiscard]] bool registry_parse_error(const char *json, char *code, size_t code_size,
                                        char *message, size_t message_size);

/* True if `version` satisfies `filter`, which may name fewer components than
   the version has: "19" matches 19.1.6, "19.2" does not. NULL or "" matches
   everything. */
[[nodiscard]] bool registry_version_matches(const char *filter, const char *version);

/* True if `filter` names a whole version rather than a family of them. It is
   what separates "I want 19.1.6" from "I want any 19", and with it the only
   thing that can select something withdrawn. */
[[nodiscard]] bool registry_version_is_exact(const char *filter);

/*
 * The artifact to install: the first one in the published order that satisfies
 * the filter.
 *
 * A withdrawn artifact is skipped, unless the filter names its exact version —
 * withdrawn means "not for new builds", not "gone", and something already
 * installed from it must stay explicable.
 *
 * False when nothing matches, which is an answer rather than a failure.
 */
[[nodiscard]] bool registry_select(const registry_artifact_list *list, const char *version_filter,
                                   registry_artifact *out);

/* What the registry calls this machine, or "" when it publishes nothing that
   could run here. */
[[nodiscard]] const char *registry_host_target(void);

/* The path segment of a kind: "toolchains", "tools", "packages". */
[[nodiscard]] const char *registry_kind_path(registry_kind kind);

/* The name of a kind, for a report: "toolchain", "tool", "package". */
[[nodiscard]] const char *registry_kind_name(registry_kind kind);

/* True if `name` can go into a URL and into a cache file name without escaping
   anything. A name arrives from the command line, and one containing ".." or a
   separator would write outside the cache. */
[[nodiscard]] bool registry_name_is_simple(const char *name);

/* Where the registry lives: the environment, then the configuration file, then
   the built-in default. Without a trailing slash, so every caller composes
   URLs the same way. */
[[nodiscard]] bool registry_base_url(char *out, size_t out_size);

/* --- asking the registry --- */

/* Every name published under one kind. Cached for an hour: `search` is the kind
   of command people run twice in a row. `refresh` discards that copy first. */
[[nodiscard]] bool registry_fetch_catalogue(registry_kind kind, bool refresh,
                                            registry_entry_list *out);
[[nodiscard]] bool registry_fetch_catalogue_watched(registry_kind kind, bool refresh,
                                                    http_tick tick, void *context,
                                                    registry_entry_list *out);

/* Every release of one name, filtered by version and target. */
[[nodiscard]] bool registry_fetch_releases(registry_kind kind, const char *name,
                                           const char *version_filter, const char *target,
                                           bool refresh, registry_artifact_list *out);
[[nodiscard]] bool registry_fetch_releases_watched(registry_kind kind, const char *name,
                                                   const char *version_filter, const char *target,
                                                   bool refresh, http_tick tick, void *context,
                                                   registry_artifact_list *out);

/*
 * Which catalogue publishes `name`, if any.
 *
 * Resolved against the catalogues rather than by asking for the name directly.
 * The transport reports a 404 and a broken network the same way, so a name that
 * does not exist is answered from something already fetched instead of from a
 * failure that cannot be told apart from an outage.
 */
[[nodiscard]] bool registry_find(const char *name, bool refresh, registry_entry *out);

/* What the registry matches for a substring. Not cached: the query is free-form
   and the answer is small. */
[[nodiscard]] bool registry_search_watched(const char *query, http_tick tick, void *context,
                                           registry_entry_list *out);

#endif /* PICKUP_REGISTRY_SOURCE_H */
