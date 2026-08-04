#include <pickup/sources/conda_source.h>

#include <pickup/model/toolchain.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/util/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Where the channel is asked what it has. */
#define PACKAGE_URL_FORMAT "https://api.anaconda.org/package/conda-forge/%s"
#define DIST_URL_FORMAT \
    "https://api.anaconda.org/dist/conda-forge/%s/%s/%s/%s"
#define DOWNLOAD_URL_FORMAT "https://conda.anaconda.org/conda-forge/%s/%s"

/* The listing is cached like the LLVM index and for the same reason: the same
   package is asked for repeatedly while a dependency closure is worked out. */
#define CACHE_FILE_FORMAT "conda-%s.json"
#define CACHE_TTL_SECONDS 3600

/* Fields of the API answer. */
#define FIELD_FILES     "files"
#define FIELD_BASENAME  "basename"
#define FIELD_VERSION   "version"
#define FIELD_SIZE      "size"
#define FIELD_ATTRS     "attrs"
#define FIELD_SUBDIR    "subdir"
#define FIELD_BUILD     "build"
#define FIELD_DEPENDS   "depends"
#define FIELD_TIMESTAMP "timestamp"
#define FIELD_SHA256    "sha256"

/* Which subdirectory of the channel this host installs from. */
#if defined(_WIN32)
#  define HOST_SUBDIR "win-64"
#elif defined(__APPLE__)
#  if defined(__aarch64__)
#    define HOST_SUBDIR "osx-arm64"
#  else
#    define HOST_SUBDIR "osx-64"
#  endif
#elif defined(__linux__) && defined(__aarch64__)
#  define HOST_SUBDIR "linux-aarch64"
#elif defined(__linux__) && defined(__x86_64__)
#  define HOST_SUBDIR "linux-64"
#else
/* Carries no compiler, so a host Pickup has no subdirectory for resolves to
   nothing rather than to a package built for somewhere else. */
#  define HOST_SUBDIR "noarch"
#endif

#define LIST_INITIAL_CAPACITY 8
#define LIST_GROWTH_FACTOR    2

const char *conda_subdir(void) {
    return HOST_SUBDIR;
}

/* --- the list --- */

void conda_list_init(conda_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void conda_list_free(conda_list *list) {
    free(list->items);
    conda_list_init(list);
}

bool conda_list_push(conda_list *list, const conda_package *package) {
    if (list->count == list->capacity) {
        size_t next = list->capacity == 0 ? LIST_INITIAL_CAPACITY
                                          : list->capacity * LIST_GROWTH_FACTOR;
        conda_package *items = realloc(list->items, next * sizeof *items);
        if (items == NULL)
            return false;
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count++] = *package;
    return true;
}

/* --- version matching --- */

/* How many components a version names: "16" is one, "16.1" two. */
static int component_count(const char *version) {
    int count = 1;
    for (const char *c = version; *c != '\0'; c++) {
        if (*c == '.')
            count++;
    }
    return count;
}

/* True if `version` satisfies `filter`, which may name fewer components than
   the version has. Same meaning as the LLVM source gives it: "16" is any 16. */
static bool version_matches(const char *filter, const char *version) {
    if (filter == NULL || filter[0] == '\0')
        return true;

    toolchain_version wanted, actual;
    if (!toolchain_version_parse(filter, &wanted)
        || !toolchain_version_parse(version, &actual))
        return false;

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

/* The file name without its directory: the API prefixes it with the subdir. */
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* Marks a version as something the project has not called finished. */
static const char *const prerelease_marks[] = { "rc", "alpha", "beta", "dev", "pre" };
#define PRERELEASE_COUNT (sizeof prerelease_marks / sizeof prerelease_marks[0])

/*
 * True if this version is a release candidate or similar.
 *
 * The channel publishes them alongside finished releases, and the newest build
 * of a package is often one — `clang-format` at the time of writing resolves to
 * 23.1.0.rc2. Installing one by accident is not something asking for the latest
 * version should be able to cause, which is the same rule the LLVM source
 * applies to GitHub's prerelease flag.
 */
static bool is_prerelease(const char *version) {
    for (size_t i = 0; i < PRERELEASE_COUNT; i++) {
        const char *found = strstr(version, prerelease_marks[i]);
        /* Preceded by a digit or a dot, so a package whose name happens to
           carry those letters is not mistaken for one. */
        if (found != NULL && found != version
            && (found[-1] == '.' || (found[-1] >= '0' && found[-1] <= '9')))
            return true;
    }
    return false;
}

/* Read the dependency list, which is what makes a closure possible at all. */
static void read_depends(json_value attrs, conda_package *out) {
    json_value depends = json_get(attrs, FIELD_DEPENDS);
    for (size_t i = 0; i < json_count(depends) && out->depend_count < CONDA_MAX_DEPENDS; i++) {
        const char *spec = json_string(json_at(depends, i));
        if (spec == NULL)
            continue;
        (void)fs_format_path(out->depends[out->depend_count++], CONDA_SPEC_MAX,
                             "%s", spec);
    }
}

/* Fill `out` from one entry of the files array. False when it is not a build
   for this host. */
static bool read_package(json_value file, const char *name, conda_package *out) {
    json_value attrs = json_get(file, FIELD_ATTRS);
    const char *subdir = json_string(json_get(attrs, FIELD_SUBDIR));
    /* Both subdirectories this host installs from. noarch is not an
       afterthought: the development halves of the compilers live there. */
    if (subdir == NULL
        || (strcmp(subdir, conda_subdir()) != 0 && strcmp(subdir, CONDA_NOARCH) != 0))
        return false;

    const char *basename = json_string(json_get(file, FIELD_BASENAME));
    const char *version = json_string(json_get(file, FIELD_VERSION));
    if (basename == NULL || version == NULL)
        return false;

    *out = (conda_package){ 0 };
    (void)fs_format_path(out->name, sizeof out->name, "%s", name);
    (void)fs_format_path(out->version, sizeof out->version, "%s", version);
    (void)fs_format_path(out->subdir, sizeof out->subdir, "%s", subdir);
    (void)fs_format_path(out->file, sizeof out->file, "%s", basename_of(basename));

    const char *build = json_string(json_get(attrs, FIELD_BUILD));
    if (build != NULL)
        (void)fs_format_path(out->build, sizeof out->build, "%s", build);

    /* Composed from the channel rather than taken from the API's own download
       field, which is a redirect through a second host. */
    (void)fs_format_path(out->url, sizeof out->url, DOWNLOAD_URL_FORMAT,
                         subdir, out->file);

    long long number = 0;
    if (json_number(json_get(file, FIELD_SIZE), &number))
        out->size = number;
    if (json_number(json_get(attrs, FIELD_TIMESTAMP), &number))
        out->timestamp = number;

    read_depends(attrs, out);
    return true;
}

/* Newest first: by version, and among equal versions by when it was built.
   Two builds of one version differ in what they were built against, and the
   later one is the one the rest of the channel now expects. */
static int compare_descending(const void *left, const void *right) {
    const conda_package *a = left;
    const conda_package *b = right;

    toolchain_version version_a, version_b;
    if (toolchain_version_parse(a->version, &version_a)
        && toolchain_version_parse(b->version, &version_b)) {
        int order = toolchain_version_compare(version_b, version_a);
        if (order != 0)
            return order;
    }
    if (b->timestamp != a->timestamp)
        return b->timestamp > a->timestamp ? 1 : -1;
    return strcmp(b->build, a->build);
}

bool conda_parse_packages(const char *json, const char *version_filter, conda_list *out) {
    conda_list_init(out);

    json_document *doc = json_parse(json);
    if (doc == NULL)
        return false;

    json_value root = json_root(doc);
    const char *name = json_string(json_get(root, "name"));
    if (name == NULL) {
        json_free(doc);
        return false;
    }

    json_value files = json_get(root, FIELD_FILES);
    bool ok = true;
    for (size_t i = 0; ok && i < json_count(files); i++) {
        conda_package package;
        if (!read_package(json_at(files, i), name, &package))
            continue;
        /* Never offered, whatever the filter says. */
        if (is_prerelease(package.version))
            continue;
        if (!version_matches(version_filter, package.version))
            continue;
        ok = conda_list_push(out, &package);
    }

    json_free(doc);
    if (!ok) {
        conda_list_free(out);
        return false;
    }

    if (out->count > 1)
        qsort(out->items, out->count, sizeof out->items[0], compare_descending);
    return true;
}

bool conda_best(const conda_list *list, conda_package *out) {
    if (list->count == 0)
        return false;
    *out = list->items[0];  /* sorted newest first */
    return true;
}

/* --- fetching --- */

static bool cache_file(const char *name, char *out, size_t out_size) {
    char directory[PICKUP_PATHS_MAX];
    if (!paths_cache(directory, sizeof directory))
        return false;
    char file[CONDA_NAME_MAX + 32];
    if (!fs_format_path(file, sizeof file, CACHE_FILE_FORMAT, name))
        return false;
    return fs_format_path(out, out_size, "%s/%s", directory, file);
}

static bool cache_is_fresh(const char *path) {
    int64_t written_ns;
    if (!fs_mtime_ns(path, &written_ns))
        return false;
    long long age = (long long)time(NULL) - (long long)(written_ns / 1000000000);
    return age >= 0 && age < CACHE_TTL_SECONDS;
}

bool conda_fetch_packages(const char *name, const char *version_filter,
                          bool refresh, conda_list *out) {
    return conda_fetch_packages_watched(name, version_filter, refresh, NULL, NULL, out);
}

bool conda_fetch_packages_watched(const char *name, const char *version_filter,
                                  bool refresh, http_tick tick, void *context,
                                  conda_list *out) {
    conda_list_init(out);
    if (name == NULL || name[0] == '\0')
        return false;

    char path[PICKUP_PATHS_MAX];
    if (!cache_file(name, path, sizeof path))
        return false;
    if (refresh)
        remove(path);

    if (!cache_is_fresh(path)) {
        char directory[PICKUP_PATHS_MAX];
        if (!paths_cache(directory, sizeof directory) || !fs_make_dirs(directory))
            return false;

        char url[CONDA_URL_MAX];
        if (!fs_format_path(url, sizeof url, PACKAGE_URL_FORMAT, name))
            return false;
        if (!http_download_watched(url, path, tick, context))
            return false;
    }

    char *text = fs_read_file(path);
    if (text == NULL)
        return false;

    bool ok = conda_parse_packages(text, version_filter, out);
    free(text);
    /* A cached answer that no longer parses is worthless; drop it so the next
       run fetches a fresh one instead of failing the same way. */
    if (!ok)
        remove(path);
    return ok;
}

bool conda_fetch_sha256(conda_package *package) {
    /* Asked for in the subdirectory the package was found in, which is not
       always the host's: a noarch package is only there. */
    char url[CONDA_URL_MAX];
    if (!fs_format_path(url, sizeof url, DIST_URL_FORMAT, package->name,
                        package->version, package->subdir, package->file))
        return false;

    char downloads[PICKUP_PATHS_MAX];
    char path[PICKUP_PATHS_MAX];
    if (!paths_cache(downloads, sizeof downloads) || !fs_make_dirs(downloads))
        return false;
    if (!fs_format_path(path, sizeof path, "%s/dist-%s.json", downloads, package->file))
        return false;

    if (!http_download(url, path))
        return false;

    char *text = fs_read_file(path);
    (void)remove(path);
    if (text == NULL)
        return false;

    json_document *doc = json_parse(text);
    free(text);
    if (doc == NULL)
        return false;

    const char *digest = json_string(json_get(json_root(doc), FIELD_SHA256));
    bool ok = digest != NULL
        && fs_format_path(package->sha256, sizeof package->sha256, "%s", digest);
    json_free(doc);
    return ok;
}

/* --- dependency specifications --- */

/* Copy the next whitespace-separated word, returning what follows it. */
static const char *next_word(const char *text, char *out, size_t out_size) {
    while (*text == ' ' || *text == '\t')
        text++;
    if (*text == '\0')
        return NULL;

    size_t length = 0;
    while (text[length] != '\0' && text[length] != ' ' && text[length] != '\t')
        length++;
    if (length >= out_size)
        return NULL;

    memcpy(out, text, length);
    out[length] = '\0';
    return text + length;
}

/* Names of virtual packages start with two underscores. */
#define VIRTUAL_PREFIX "__"

bool conda_is_virtual(const char *name) {
    return name != NULL
        && strncmp(name, VIRTUAL_PREFIX, sizeof VIRTUAL_PREFIX - 1) == 0;
}

/* True if every character could belong to a version.

   toolchain_version_parse reads as far as it understands and stops, so on its
   own it accepts ">=1.0|<2.0" by reading the 1.0 and ignoring the rest. That
   is the dangerous kind of leniency: it turns an operator Pickup does not
   implement into a constraint it silently widens. */
static bool looks_like_version(const char *text) {
    for (const char *c = text; *c != '\0'; c++) {
        bool allowed = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z')
                    || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '_'
                    || *c == '+' || *c == '-';
        if (!allowed)
            return false;
    }
    return text[0] != '\0';
}

/* Copy one bound of a constraint into `out`, dropping a trailing ".*" — which
   means the components named and anything below them, exactly what a partial
   version filter already means. False when it is not a version at all. */
static bool read_bound(const char *text, size_t length, char *out, size_t out_size) {
    if (length >= 2 && strncmp(text + length - 2, ".*", 2) == 0)
        length -= 2;
    if (length == 0 || length >= out_size)
        return false;
    memcpy(out, text, length);
    out[length] = '\0';

    if (!looks_like_version(out))
        return false;

    toolchain_version parsed;
    return toolchain_version_parse(out, &parsed);
}

bool conda_spec_parse(const char *text, conda_spec *out) {
    if (text == NULL)
        return false;
    *out = (conda_spec){ 0 };

    const char *cursor = next_word(text, out->name, sizeof out->name);
    if (cursor == NULL || out->name[0] == '\0')
        return false;

    char constraint[CONDA_VERSION_MAX];
    cursor = next_word(cursor, constraint, sizeof constraint);
    if (cursor == NULL)
        return true;  /* a bare name: any build will do */

    /* A comma separates the two halves of a range: ">=2.17,<3.0.a0". */
    char *comma = strchr(constraint, ',');
    const char *upper = NULL;
    if (comma != NULL) {
        *comma = '\0';
        upper = comma + 1;
    }

    const char *lower = constraint;
    if (strncmp(lower, ">=", 2) == 0) {
        out->at_least = true;
        lower += 2;
    } else if (strncmp(lower, "==", 2) == 0) {
        /* Exactly this version. The channel writes it both ways. */
        lower += 2;
    } else if (lower[0] == '=') {
        lower++;
    }
    if (!read_bound(lower, strlen(lower), out->version, sizeof out->version))
        return false;

    if (upper != NULL) {
        /* Only the exclusive form appears in this channel's compiler
           packages; anything else is refused rather than widened. */
        if (upper[0] != '<')
            return false;
        upper++;
        if (!read_bound(upper, strlen(upper), out->version_below,
                        sizeof out->version_below))
            return false;
    }

    (void)next_word(cursor, out->build, sizeof out->build);
    return true;
}

bool conda_spec_matches(const conda_spec *spec, const conda_package *package) {
    if (strcmp(spec->name, package->name) != 0)
        return false;
    if (spec->version[0] == '\0')
        return true;

    toolchain_version wanted, actual;
    if (!toolchain_version_parse(spec->version, &wanted)
        || !toolchain_version_parse(package->version, &actual))
        return false;

    if (spec->at_least) {
        if (toolchain_version_compare(actual, wanted) < 0)
            return false;
    } else if (!version_matches(spec->version, package->version)) {
        return false;
    }

    /* The upper bound is exclusive, which is how conda-forge writes the end of
       a compatible range. */
    if (spec->version_below[0] != '\0') {
        toolchain_version below;
        if (!toolchain_version_parse(spec->version_below, &below))
            return false;
        if (toolchain_version_compare(actual, below) >= 0)
            return false;
    }

    /* A named build is exact: it is how the channel pins two packages that
       were built against each other. */
    return spec->build[0] == '\0' || strcmp(spec->build, package->build) == 0;
}
