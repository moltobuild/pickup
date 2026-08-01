#include <pickup/sources/llvm_source.h>

#include <pickup/model/toolchain.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/util/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Where releases come from. Thirty covers several years of LLVM at its usual
   cadence, and the answer is already megabytes at that size. */
#define RELEASES_URL \
    "https://api.github.com/repos/llvm/llvm-project/releases?per_page=30"

/* The cached copy of that answer, and how long it is trusted. */
#define RELEASES_CACHE_FILE "releases.json"
#define RELEASES_CACHE_TTL_SECONDS 3600

/* Tags are the version with this in front. */
#define TAG_PREFIX "llvmorg-"

/*
 * What this host's archive is called. Two spellings per platform, because the
 * naming changed: 18.1.8 shipped clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04
 * while 22.1.8 ships LLVM-22.1.8-Linux-X64.
 */
#if defined(_WIN32)
#  define HOST_TOKEN_NEW "Windows-X64"
#  define HOST_TOKEN_OLD "x86_64-pc-windows-msvc"
#elif defined(__APPLE__)
#  if defined(__aarch64__)
#    define HOST_TOKEN_NEW "macOS-ARM64"
#    define HOST_TOKEN_OLD "arm64-apple-darwin"
#  else
#    define HOST_TOKEN_NEW "macOS-X64"
#    define HOST_TOKEN_OLD "x86_64-apple-darwin"
#  endif
#elif defined(__aarch64__)
#  define HOST_TOKEN_NEW "Linux-ARM64"
#  define HOST_TOKEN_OLD "aarch64-linux"
#else
#  define HOST_TOKEN_NEW "Linux-X64"
#  define HOST_TOKEN_OLD "x86_64-linux"
#endif

/* Names that are not a toolchain even when they carry the host's token. */
static const char *const excluded_fragments[] = {
    ".src.", "_doxygen", "test-suite", ".sig", ".jsonl", ".exe", ".msi",
};
#define EXCLUDED_COUNT (sizeof excluded_fragments / sizeof excluded_fragments[0])

/* Archive endings worth downloading. */
static const char *const archive_suffixes[] = { ".tar.xz", ".tar.gz" };
#define SUFFIX_COUNT (sizeof archive_suffixes / sizeof archive_suffixes[0])

#define LIST_INITIAL_CAPACITY 8
#define LIST_GROWTH_FACTOR    2

/* --- the list --- */

void release_list_init(release_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void release_list_free(release_list *list) {
    free(list->items);
    release_list_init(list);
}

static bool list_push(release_list *list, const release_asset *asset) {
    if (list->count == list->capacity) {
        size_t next = list->capacity == 0 ? LIST_INITIAL_CAPACITY
                                          : list->capacity * LIST_GROWTH_FACTOR;
        release_asset *items = realloc(list->items, next * sizeof *items);
        if (items == NULL)
            return false;
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count++] = *asset;
    return true;
}

/* --- asset selection --- */

static bool has_suffix(const char *text, const char *suffix) {
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length
        && strcmp(text + text_length - suffix_length, suffix) == 0;
}

static bool is_archive(const char *name) {
    for (size_t i = 0; i < SUFFIX_COUNT; i++) {
        if (has_suffix(name, archive_suffixes[i]))
            return true;
    }
    return false;
}

static bool is_excluded(const char *name) {
    for (size_t i = 0; i < EXCLUDED_COUNT; i++) {
        if (strstr(name, excluded_fragments[i]) != NULL)
            return true;
    }
    return false;
}

bool llvm_asset_matches_host(const char *name) {
    if (name == NULL || !is_archive(name) || is_excluded(name))
        return false;
    return strstr(name, HOST_TOKEN_NEW) != NULL || strstr(name, HOST_TOKEN_OLD) != NULL;
}

/* --- version matching --- */

/* How many components a version string spells out: "14" is one, "14.2" two. */
static int component_count(const char *version) {
    int count = 1;
    for (const char *c = version; *c != '\0'; c++) {
        if (*c == '.')
            count++;
    }
    return count;
}

bool llvm_version_matches(const char *filter, const char *version) {
    if (filter == NULL || filter[0] == '\0')
        return true;

    toolchain_version wanted;
    toolchain_version actual;
    if (!toolchain_version_parse(filter, &wanted)
        || !toolchain_version_parse(version, &actual))
        return false;

    /* Only the components the filter actually named are compared, so "14"
       means any 14, not 14.0.0. */
    int named = component_count(filter);
    if (wanted.major != actual.major)
        return false;
    if (named >= 2 && wanted.minor != actual.minor)
        return false;
    if (named >= 3 && wanted.patch != actual.patch)
        return false;
    return true;
}

/* --- parsing --- */

/* "llvmorg-22.1.8" -> "22.1.8". False if the tag is not shaped that way. */
static bool version_from_tag(const char *tag, char *out, size_t out_size) {
    size_t prefix_length = strlen(TAG_PREFIX);
    if (strncmp(tag, TAG_PREFIX, prefix_length) != 0)
        return false;
    return fs_format_path(out, out_size, "%s", tag + prefix_length);
}

/* Strip the "sha256:" the API puts in front of the digest. */
static const char *digest_hex(const char *digest) {
    static const char prefix[] = "sha256:";
    if (digest == NULL)
        return NULL;
    if (strncmp(digest, prefix, sizeof prefix - 1) == 0)
        return digest + sizeof prefix - 1;
    return digest;
}

/* Fill `out` from the first asset of `release` that this host can run. */
static bool asset_of_release(json_value release, const char *version,
                             const char *tag, release_asset *out) {
    json_value assets = json_get(release, "assets");
    for (size_t i = 0; i < json_count(assets); i++) {
        json_value asset = json_at(assets, i);
        const char *name = json_string(json_get(asset, "name"));
        if (!llvm_asset_matches_host(name))
            continue;

        const char *url = json_string(json_get(asset, "browser_download_url"));
        if (url == NULL)
            continue;

        *out = (release_asset){ 0 };
        (void)fs_format_path(out->version, sizeof out->version, "%s", version);
        (void)fs_format_path(out->tag, sizeof out->tag, "%s", tag);
        (void)fs_format_path(out->asset, sizeof out->asset, "%s", name);
        (void)fs_format_path(out->url, sizeof out->url, "%s", url);

        const char *hex = digest_hex(json_string(json_get(asset, "digest")));
        if (hex != NULL)
            (void)fs_format_path(out->sha256, sizeof out->sha256, "%s", hex);

        long long size = 0;
        if (json_number(json_get(asset, "size"), &size))
            out->size = size;
        return true;
    }
    return false;
}

/* Newest first, so the default choice is the top of the list. */
static int compare_descending(const void *left, const void *right) {
    const release_asset *a = left;
    const release_asset *b = right;
    toolchain_version version_a, version_b;
    if (!toolchain_version_parse(a->version, &version_a)
        || !toolchain_version_parse(b->version, &version_b))
        return strcmp(b->version, a->version);
    return toolchain_version_compare(version_b, version_a);
}

bool llvm_parse_releases(const char *json, const char *version_filter, release_list *out) {
    release_list_init(out);

    json_document *doc = json_parse(json);
    if (doc == NULL)
        return false;

    json_value releases = json_root(doc);
    bool ok = true;
    for (size_t i = 0; ok && i < json_count(releases); i++) {
        json_value release = json_at(releases, i);

        /* Never offer a release candidate: a version filter must not be able
           to select something the project has not called finished. */
        bool prerelease = false;
        if (json_bool(json_get(release, "prerelease"), &prerelease) && prerelease)
            continue;
        bool draft = false;
        if (json_bool(json_get(release, "draft"), &draft) && draft)
            continue;

        const char *tag = json_string(json_get(release, "tag_name"));
        if (tag == NULL)
            continue;

        char version[RELEASE_VERSION_MAX];
        if (!version_from_tag(tag, version, sizeof version))
            continue;
        if (!llvm_version_matches(version_filter, version))
            continue;

        release_asset asset;
        if (asset_of_release(release, version, tag, &asset))
            ok = list_push(out, &asset);
    }

    json_free(doc);
    if (!ok) {
        release_list_free(out);
        return false;
    }

    if (out->count > 1)
        qsort(out->items, out->count, sizeof out->items[0], compare_descending);
    return true;
}

/* --- fetching --- */

/* Where the API answer is kept between runs. */
static bool cache_file(char *out, size_t out_size) {
    char downloads[PICKUP_PATHS_MAX];
    if (!paths_downloads(downloads, sizeof downloads))
        return false;
    return fs_format_path(out, out_size, "%s/%s", downloads, RELEASES_CACHE_FILE);
}

static bool cache_is_fresh(const char *path) {
    int64_t written_ns;
    if (!fs_mtime_ns(path, &written_ns))
        return false;
    long long age = (long long)time(NULL) - (long long)(written_ns / 1000000000);
    return age >= 0 && age < RELEASES_CACHE_TTL_SECONDS;
}

bool llvm_fetch_releases(const char *version_filter, release_list *out) {
    release_list_init(out);

    char path[PICKUP_PATHS_MAX];
    if (!cache_file(path, sizeof path))
        return false;

    if (!cache_is_fresh(path)) {
        char directory[PICKUP_PATHS_MAX];
        if (!paths_downloads(directory, sizeof directory) || !fs_make_dirs(directory))
            return false;
        if (!http_download(RELEASES_URL, path))
            return false;
    }

    char *text = fs_read_file(path);
    if (text == NULL)
        return false;

    bool ok = llvm_parse_releases(text, version_filter, out);
    free(text);
    /* A cached answer that no longer parses is worthless; drop it so the next
       run fetches a fresh one instead of failing again. */
    if (!ok)
        remove(path);
    return ok;
}

bool llvm_best(const release_list *list, release_asset *out) {
    if (list->count == 0)
        return false;
    *out = list->items[0]; /* the list is sorted newest first */
    return true;
}
