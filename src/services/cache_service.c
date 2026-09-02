#include <pickup/services/cache_service.h>

#include <pickup/detect/capability.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/util/hash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Bumped whenever the record layout changes. An older file is discarded
   rather than migrated: rebuilding it costs one scan. */
#define CACHE_FORMAT_VERSION 3

/* The cache file, inside the directory Pickup keeps regenerable state in. */
#define CACHE_FILENAME "toolchains"

/* Size of the buffers holding the cache path and one record. */
#define CACHE_PATH_SIZE 4096
#define CACHE_RECORD_SIZE 8192

/* Fields of a record are tab-separated: paths may contain spaces, never tabs. */
#define FIELD_SEPARATOR '\t'

/* Compose the cache file path. False if there is no home directory to use. */
static bool cache_path(char *out, size_t out_size) {
    char directory[CACHE_PATH_SIZE];
    if (!paths_cache(directory, sizeof directory))
        return false;
    return fs_format_path(out, out_size, "%s/%s", directory, CACHE_FILENAME);
}

/* The signature that decides whether a cached entry still describes the binary
   on disk: replacing a compiler changes its size, its timestamp, or both. */
static bool binary_signature(const char *path, long long *mtime, long long *size) {
    struct stat info;
    if (stat(path, &info) != 0)
        return false;
    *mtime = (long long)info.st_mtime;
    *size = (long long)info.st_size;
    return true;
}

/* Read one tab-separated field into `out`, advancing `cursor` past it. */
static bool next_field(const char **cursor, char *out, size_t out_size) {
    const char *start = *cursor;
    const char *end = strchr(start, FIELD_SEPARATOR);
    size_t length = end != NULL ? (size_t)(end - start) : strlen(start);
    if (length >= out_size)
        return false;
    memcpy(out, start, length);
    out[length] = '\0';
    *cursor = end != NULL ? end + 1 : start + length;
    return true;
}

/* Parse one record, and confirm the binary it describes has not changed. */
static bool parse_record(const char *line, toolchain *out) {
    memset(out, 0, sizeof *out);

    char field[CACHE_PATH_SIZE];
    const char *cursor = line;

    if (!next_field(&cursor, out->path, sizeof out->path) ||
        !next_field(&cursor, out->name, sizeof out->name) ||
        !next_field(&cursor, out->cxx_path, sizeof out->cxx_path) ||
        !next_field(&cursor, field, sizeof field))
        return false;
    out->vendor = toolchain_vendor_parse(field);

    if (!next_field(&cursor, field, sizeof field) || !toolchain_version_parse(field, &out->version))
        return false;
    if (!next_field(&cursor, out->target, sizeof out->target))
        return false;

    if (!next_field(&cursor, field, sizeof field))
        return false;
    out->c_features.bits = strtoull(field, NULL, 10);
    if (!next_field(&cursor, field, sizeof field))
        return false;
    out->cxx_features.bits = strtoull(field, NULL, 10);

    /* The recorded signature must still match the file, or the compiler was
       replaced and everything probed about it is suspect. */
    long long cached_mtime = 0, cached_size = 0;
    if (!next_field(&cursor, field, sizeof field))
        return false;
    cached_mtime = strtoll(field, NULL, 10);
    if (!next_field(&cursor, field, sizeof field))
        return false;
    cached_size = strtoll(field, NULL, 10);

    long long mtime = 0, size = 0;
    if (!binary_signature(out->path, &mtime, &size))
        return false;
    return mtime == cached_mtime && size == cached_size;
}

/* A fingerprint of the candidate set.

   It cannot be derived from the cached toolchains: the scan also turns up
   files that are not compilers at all, and those never reach the inventory.
   Comparing the two sets would always differ, so the set actually seen is
   recorded instead. Installing or removing any candidate changes this. */
static unsigned long long candidates_fingerprint(const str_list *candidates) {
    unsigned long long hash = HASH_INIT;
    for (size_t i = 0; i < str_list_count(candidates); i++)
        hash = hash_add(hash, str_list_get(candidates, i));
    return hash;
}

bool cache_load(const str_list *candidates, inventory *out) {
    memset(out, 0, sizeof *out);

    char path[CACHE_PATH_SIZE];
    if (!cache_path(path, sizeof path))
        return false;

    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return false;

    char line[CACHE_RECORD_SIZE];
    int version = 0;
    unsigned long long candidate_mark = 0;
    unsigned long long catalog_mark = 0;
    /* The catalog fingerprint has to match as well: the records store features
       as bit positions, so a catalog that changed makes every stored bit mean
       something else. */
    bool ok = fgets(line, sizeof line, file) != NULL &&
              sscanf(line, "%d %llu %llu", &version, &candidate_mark, &catalog_mark) == 3 &&
              version == CACHE_FORMAT_VERSION &&
              candidate_mark == candidates_fingerprint(candidates) &&
              catalog_mark == capability_catalog_fingerprint();

    while (ok && fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0')
            continue;
        toolchain chain;
        if (!parse_record(line, &chain)) {
            ok = false;
            break;
        }
        ok = inventory_append(out, &chain);
    }
    fclose(file);

    if (!ok)
        inventory_free(out);
    return ok;
}

bool cache_store(const str_list *candidates, const inventory *list) {
    char path[CACHE_PATH_SIZE];
    if (!cache_path(path, sizeof path))
        return false;

    /* Create the directory chain the first time round. */
    char directory[CACHE_PATH_SIZE];
    if (!fs_format_path(directory, sizeof directory, "%s", path))
        return false;
    char *slash = strrchr(directory, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (!fs_make_dirs(directory))
            return false;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;

    bool ok = fprintf(file, "%d %llu %llu\n", CACHE_FORMAT_VERSION,
                      candidates_fingerprint(candidates), capability_catalog_fingerprint()) > 0;
    for (size_t i = 0; ok && i < list->count; i++) {
        const toolchain *chain = &list->items[i];
        char version[32];
        toolchain_version_format(chain->version, version, sizeof version);

        long long mtime = 0, size = 0;
        if (!binary_signature(chain->path, &mtime, &size))
            continue; /* it vanished mid-scan; simply do not record it */

        ok = fprintf(file, "%s\t%s\t%s\t%s\t%s\t%s\t%llu\t%llu\t%lld\t%lld\n", chain->path,
                     chain->name, chain->cxx_path, toolchain_vendor_name(chain->vendor), version,
                     chain->target, (unsigned long long)chain->c_features.bits,
                     (unsigned long long)chain->cxx_features.bits, mtime, size) > 0;
    }
    return fclose(file) == 0 && ok;
}

void cache_discard(void) {
    char path[CACHE_PATH_SIZE];
    if (cache_path(path, sizeof path))
        remove(path);
}
