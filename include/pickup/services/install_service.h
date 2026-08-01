#ifndef PICKUP_INSTALL_SERVICE_H
#define PICKUP_INSTALL_SERVICE_H

#include <stdbool.h>

#include <pickup/model/toolchain.h>
#include <pickup/services/paths_service.h>
#include <pickup/sources/llvm_source.h>
#include <pickup/util/sha256.h>

/*
 * Installing a downloaded toolchain.
 *
 * Everything lands under the pickup home, so nothing here ever needs
 * administrator rights. The order is download, verify, unpack, and only then
 * take the final name: an archive that fails its digest is discarded without
 * being unpacked, and a half-extracted tree is assembled out of the way and
 * renamed into place, which is atomic, so an interrupted install leaves no
 * directory that looks finished.
 *
 * What comes out is probed rather than assumed. The version and target in the
 * directory name are read from the installed compiler itself, which also
 * proves that what was unpacked actually runs.
 */

typedef enum {
    install_ok,
    install_ok_unverified,   /* installed, but the source published no digest */
    install_ok_unpruned,     /* the minimal profile did not work; installed whole */
    install_no_downloader,   /* curl is missing */
    install_no_extractor,    /* tar is missing */
    install_unverifiable,    /* no digest, and the caller did not allow it */
    install_download_failed,
    install_hash_mismatch,
    install_extract_failed,
    install_not_a_toolchain, /* unpacked, but nothing in it identifies itself */
    install_path_error,
} install_status;

typedef struct {
    install_status status;
    char directory[PICKUP_PATHS_MAX];  /* where it landed, when it landed */
    char expected[SHA256_HEX_SIZE];    /* digests, on a mismatch */
    char actual[SHA256_HEX_SIZE];
    toolchain installed;               /* what the compiler said it is */
    size_t features_proven;            /* what it compiled once installed */
    long long installed_size;          /* bytes on disk */
} install_report;

typedef struct {
    const release_asset *asset;
    bool allow_unverified;  /* install even when there is no digest to check */
    bool full;              /* unpack the whole release instead of the profile */
} install_request;

/* Run the whole thing. Never partially applies: on any failure nothing is left
   in the toolchains directory. */
[[nodiscard]] install_report install_run(const install_request *request);

/* A one-line explanation of a status, for a caller that reports to a user. */
[[nodiscard]] const char *install_status_message(install_status status);

/* True if the toolchain ended up installed, verified or not. */
[[nodiscard]] bool install_succeeded(install_status status);

#endif /* PICKUP_INSTALL_SERVICE_H */
