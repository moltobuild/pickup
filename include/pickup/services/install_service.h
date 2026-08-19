#ifndef PICKUP_INSTALL_SERVICE_H
#define PICKUP_INSTALL_SERVICE_H

#include <stdbool.h>

#include <pickup/model/toolchain.h>
#include <pickup/services/paths_service.h>
#include <pickup/sources/registry_source.h>
#include <pickup/util/sha256.h>

/*
 * Installing what the registry publishes.
 *
 * Everything lands under the pickup home, so nothing here ever needs
 * administrator rights. The order is download, verify, unpack, prove, and only
 * then take the final name: an archive that fails its digest is discarded
 * without being unpacked, and a half-extracted tree is assembled out of the way
 * and renamed into place, which is atomic, so an interrupted install leaves no
 * directory that looks finished.
 *
 * What comes out is probed rather than assumed. The version and target in the
 * directory name are read from the installed compiler itself, which also proves
 * that what was unpacked actually runs — and the flags it needs are discovered
 * here rather than taken from what the registry published about them, because
 * those describe the machine that built the artifact.
 */

typedef enum {
    install_ok,
    install_no_downloader,      /* curl is missing */
    install_no_extractor,       /* tar is missing */
    install_no_zstd,            /* tar cannot open what the registry packs */
    install_unsupported_format, /* packed in a way this build cannot open */
    install_yanked,             /* withdrawn, and not asked for by its version */
    install_download_failed,
    install_hash_mismatch,
    install_extract_failed,
    install_not_a_toolchain, /* unpacked, but nothing in it compiles */
    install_not_a_tool,      /* unpacked, but the binary it named said nothing */
    install_path_error,
} install_status;

typedef struct {
    install_status status;
    char directory[PICKUP_PATHS_MAX]; /* where it landed, when it landed */
    char expected[SHA256_HEX_SIZE];   /* digests, on a mismatch */
    char actual[SHA256_HEX_SIZE];
    toolchain installed;      /* what the compiler said it is; empty for a tool */
    size_t features_proven;   /* what it compiled once installed */
    long long installed_size; /* bytes on disk */
} install_report;

typedef struct {
    const registry_artifact *artifact;
    /*
     * Install something the registry withdrew.
     *
     * Decided by the caller, because the question is whether the user named
     * that exact version, and the command is what knows how it was asked.
     */
    bool allow_yanked;
} install_request;

/*
 * Run the whole thing, for a toolchain or for a tool.
 *
 * Which one it is changes what has to be proved at the end, not how it gets
 * there: a compiler has to compile, link and run something, and a formatter has
 * nothing to compile, so what stands in for it is that the binary answers.
 * Nothing is adopted on the strength of having unpacked.
 *
 * Never partially applies: on any failure nothing is left installed.
 */
[[nodiscard]] install_report install_run(const install_request *request);

/* A one-line explanation of a status, for a caller that reports to a user. */
[[nodiscard]] const char *install_status_message(install_status status);

/* True if it ended up installed. There is only one such outcome now: the
   registry publishes a digest for everything, so there is no such thing as an
   install that went through unverified. */
[[nodiscard]] bool install_succeeded(install_status status);

#endif /* PICKUP_INSTALL_SERVICE_H */
