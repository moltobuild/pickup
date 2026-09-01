#include <pickup/sources/registry_source.h>

#include <pickup/model/toolchain.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/http_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/services/preference_service.h>
#include <pickup/util/json.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The API this speaks. Everything below is composed against it, so the version
   appears once. */
#define API_PREFIX "v1"

/* Cached index files, and how long they are trusted. */
#define CATALOGUE_CACHE_FORMAT "registry-%s.json"
#define RELEASES_CACHE_FORMAT "registry-%s-%s.json"
#define SEARCH_CACHE_FILE "registry-search.json"
#define REGISTRY_CACHE_TTL_SECONDS 3600

/*
 * What the registry calls this machine.
 *
 * A guess about someone else's naming, and treated as one: what actually
 * decides is whether an artifact carrying this target comes back. A host with
 * no name here asks for nothing rather than asking for the wrong thing.
 */
#if defined(_WIN32)
#define HOST_TARGET "windows-x86_64"
#elif defined(__APPLE__) && defined(__aarch64__)
#define HOST_TARGET "darwin-aarch64"
#elif defined(__APPLE__)
#define HOST_TARGET "darwin-x86_64"
#elif defined(__linux__) && defined(__aarch64__)
#define HOST_TARGET "linux-aarch64"
#elif defined(__linux__) && defined(__x86_64__)
#define HOST_TARGET "linux-x86_64"
#else
#define HOST_TARGET ""
#endif

#define LIST_INITIAL_CAPACITY 8
#define LIST_GROWTH_FACTOR 2

/* --- the lists --- */

void registry_artifact_list_init(registry_artifact_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void registry_artifact_list_free(registry_artifact_list *list) {
    free(list->items);
    registry_artifact_list_init(list);
}

static bool artifact_push(registry_artifact_list *list, const registry_artifact *item) {
    if (list->count == list->capacity) {
        size_t next =
            list->capacity == 0 ? LIST_INITIAL_CAPACITY : list->capacity * LIST_GROWTH_FACTOR;
        registry_artifact *items = realloc(list->items, next * sizeof *items);
        if (items == NULL)
            return false;
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count++] = *item;
    return true;
}

void registry_entry_list_init(registry_entry_list *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void registry_entry_list_free(registry_entry_list *list) {
    free(list->items);
    registry_entry_list_init(list);
}

static bool entry_push(registry_entry_list *list, const registry_entry *item) {
    if (list->count == list->capacity) {
        size_t next =
            list->capacity == 0 ? LIST_INITIAL_CAPACITY : list->capacity * LIST_GROWTH_FACTOR;
        registry_entry *items = realloc(list->items, next * sizeof *items);
        if (items == NULL)
            return false;
        list->items = items;
        list->capacity = next;
    }
    list->items[list->count++] = *item;
    return true;
}

bool registry_entry_list_push(registry_entry_list *list, const registry_entry *item) {
    return entry_push(list, item);
}

/* --- kinds --- */

const char *registry_kind_path(registry_kind kind) {
    switch (kind) {
    case registry_kind_tool:
        return "tools";
    case registry_kind_package:
        return "packages";
    default:
        return "toolchains";
    }
}

const char *registry_kind_name(registry_kind kind) {
    switch (kind) {
    case registry_kind_tool:
        return "tool";
    case registry_kind_package:
        return "package";
    default:
        return "toolchain";
    }
}

/* The kind a document names, falling back to what the caller was asking for:
   an answer that omits it is still about the thing that was requested. */
static registry_kind kind_of(const char *text, registry_kind fallback) {
    if (text == NULL)
        return fallback;
    if (strcmp(text, "tool") == 0)
        return registry_kind_tool;
    if (strcmp(text, "package") == 0)
        return registry_kind_package;
    if (strcmp(text, "toolchain") == 0)
        return registry_kind_toolchain;
    return fallback;
}

/* --- reading what the registry says --- */

/* Copy a JSON string into a fixed field. False when there was no string, so a
   caller can tell a missing required field from an empty one. */
static bool copy_string(json_value value, char *out, size_t out_size) {
    const char *text = json_string(value);
    if (text == NULL)
        return false;
    return fs_format_path(out, out_size, "%s", text);
}

/* The same, for a field whose absence is not an error. */
static void copy_optional(json_value value, char *out, size_t out_size) {
    (void)copy_string(value, out, out_size);
}

static void read_provides(json_value metadata, registry_artifact *out) {
    json_value provides = json_get(metadata, "provides");
    size_t count = json_count(provides);
    for (size_t i = 0; i < count && out->provides_count < REGISTRY_FEATURES_MAX; i++) {
        char *slot = out->provides[out->provides_count];
        if (copy_string(json_at(provides, i), slot, REGISTRY_FEATURE_MAX))
            out->provides_count++;
    }
}

/* The descriptive half of an artifact: what it says about itself. None of it is
   required, and none of it configures anything. */
static void read_metadata(json_value artifact, registry_artifact *out) {
    json_value metadata = json_get(artifact, "metadata");
    if (!json_is_valid(metadata))
        return;

    read_provides(metadata, out);
    copy_optional(json_get(json_get(metadata, "about"), "description"), out->description,
                  sizeof out->description);

    json_value toolchain = json_get(metadata, "toolchain");
    copy_optional(json_get(toolchain, "vendor"), out->vendor, sizeof out->vendor);
    copy_optional(json_get(toolchain, "triple"), out->triple, sizeof out->triple);
    copy_optional(json_get(toolchain, "c_driver"), out->c_driver, sizeof out->c_driver);

    copy_optional(json_get(json_get(metadata, "tool"), "binary"), out->binary, sizeof out->binary);
}

/*
 * Read one artifact.
 *
 * Everything the schema marks required is required: a blob that cannot be
 * verified, located or sized is not something to carry forward on the chance
 * that the rest of the pipeline copes.
 */
static bool read_artifact(json_value value, registry_kind fallback, registry_artifact *out) {
    *out = (registry_artifact){0};
    out->kind = kind_of(json_string(json_get(value, "kind")), fallback);

    if (!copy_string(json_get(value, "name"), out->name, sizeof out->name) ||
        !copy_string(json_get(value, "version"), out->version, sizeof out->version) ||
        !copy_string(json_get(value, "target"), out->target, sizeof out->target) ||
        !copy_string(json_get(value, "format"), out->format, sizeof out->format) ||
        !copy_string(json_get(value, "checksum"), out->checksum, sizeof out->checksum) ||
        !copy_string(json_get(value, "download_url"), out->download_url, sizeof out->download_url))
        return false;

    long long size = 0;
    if (!json_number(json_get(value, "size_bytes"), &size) || size < 0)
        return false;
    out->size_bytes = size;

    (void)json_bool(json_get(value, "yanked"), &out->yanked);
    copy_optional(json_get(value, "published_at"), out->published_at, sizeof out->published_at);
    copy_optional(json_get(value, "published_by"), out->published_by, sizeof out->published_by);
    read_metadata(value, out);
    return true;
}

bool registry_parse_artifact(const char *json, registry_artifact *out) {
    json_document *doc = json_parse(json);
    if (doc == NULL)
        return false;
    bool ok = read_artifact(json_root(doc), registry_kind_toolchain, out);
    json_free(doc);
    return ok;
}

/* --- version matching --- */

/* How many components a version string spells out: "19" is one, "19.1" two. */
static int component_count(const char *version) {
    int count = 1;
    for (const char *c = version; *c != '\0'; c++) {
        if (*c == '.')
            count++;
    }
    return count;
}

bool registry_version_matches(const char *filter, const char *version) {
    if (filter == NULL || filter[0] == '\0')
        return true;
    if (version == NULL)
        return false;

    toolchain_version wanted;
    toolchain_version actual;
    if (!toolchain_version_parse(filter, &wanted) || !toolchain_version_parse(version, &actual))
        return false;

    /* Only the components the filter actually named are compared, so "19"
       means any 19, not 19.0.0. */
    int named = component_count(filter);
    if (wanted.major != actual.major)
        return false;
    if (named >= 2 && wanted.minor != actual.minor)
        return false;
    if (named >= 3 && wanted.patch != actual.patch)
        return false;
    return true;
}

bool registry_version_is_exact(const char *filter) {
    if (filter == NULL || filter[0] == '\0')
        return false;
    toolchain_version parsed;
    if (!toolchain_version_parse(filter, &parsed))
        return false;
    return component_count(filter) >= 3;
}

bool registry_select(const registry_artifact_list *list, const char *version_filter,
                     registry_artifact *out) {
    bool exact = registry_version_is_exact(version_filter);
    for (size_t i = 0; i < list->count; i++) {
        const registry_artifact *item = &list->items[i];
        if (!registry_version_matches(version_filter, item->version))
            continue;
        /* Withdrawn is not gone: it can still be named, and only named. */
        if (item->yanked && !exact)
            continue;
        *out = *item;
        return true;
    }
    return false;
}

/* --- parsing documents --- */

bool registry_parse_releases(const char *json, const char *version_filter, const char *target,
                             registry_artifact_list *out) {
    registry_artifact_list_init(out);

    json_document *doc = json_parse(json);
    if (doc == NULL)
        return false;

    json_value root = json_root(doc);
    registry_kind kind = kind_of(json_string(json_get(root, "kind")), registry_kind_toolchain);

    /* Either a list of releases or one release: the coordinate endpoints answer
       with the same shape one level down. */
    json_value releases = json_get(root, "releases");
    bool single = !json_is_valid(releases);

    bool ok = true;
    size_t release_count = single ? 1 : json_count(releases);
    for (size_t i = 0; ok && i < release_count; i++) {
        json_value release = single ? root : json_at(releases, i);
        json_value targets = json_get(release, "targets");

        for (size_t j = 0; ok && j < json_count(targets); j++) {
            registry_artifact artifact;
            if (!read_artifact(json_at(targets, j), kind, &artifact))
                continue;
            if (!registry_version_matches(version_filter, artifact.version))
                continue;
            if (target != NULL && strcmp(target, artifact.target) != 0)
                continue;
            ok = artifact_push(out, &artifact);
        }
    }

    json_free(doc);
    if (!ok) {
        registry_artifact_list_free(out);
        return false;
    }
    return true;
}

/* One catalogue row. `targets` is a list of names in a catalogue and a count in
   a search result, so each caller reads the shape it gets. */
static bool read_entry(json_value value, registry_kind fallback, registry_entry *out) {
    *out = (registry_entry){0};
    out->kind = kind_of(json_string(json_get(value, "kind")), fallback);

    if (!copy_string(json_get(value, "name"), out->name, sizeof out->name) ||
        !copy_string(json_get(value, "latest_version"), out->latest_version,
                     sizeof out->latest_version))
        return false;

    long long versions = 0;
    if (json_number(json_get(value, "versions"), &versions) && versions > 0)
        out->versions = (size_t)versions;

    json_value targets = json_get(value, "targets");
    if (json_type_of(targets) == json_type_array) {
        size_t count = json_count(targets);
        for (size_t i = 0; i < count && out->target_count < REGISTRY_TARGETS_MAX; i++) {
            char *slot = out->targets[out->target_count];
            if (copy_string(json_at(targets, i), slot, REGISTRY_TARGET_MAX))
                out->target_count++;
        }
    }

    copy_optional(json_get(value, "published_at"), out->published_at, sizeof out->published_at);
    copy_optional(json_get(value, "published_by"), out->published_by, sizeof out->published_by);
    return true;
}

/* Both index documents are a kind plus an array of rows; only the field holding
   the rows differs. */
static bool parse_entries(const char *json, const char *field, registry_entry_list *out) {
    registry_entry_list_init(out);

    json_document *doc = json_parse(json);
    if (doc == NULL)
        return false;

    json_value root = json_root(doc);
    registry_kind kind = kind_of(json_string(json_get(root, "kind")), registry_kind_toolchain);
    json_value rows = json_get(root, field);

    bool ok = true;
    for (size_t i = 0; ok && i < json_count(rows); i++) {
        registry_entry entry;
        if (read_entry(json_at(rows, i), kind, &entry))
            ok = entry_push(out, &entry);
    }

    json_free(doc);
    if (!ok) {
        registry_entry_list_free(out);
        return false;
    }
    return true;
}

bool registry_parse_catalogue(const char *json, registry_entry_list *out) {
    return parse_entries(json, "entries", out);
}

bool registry_parse_search(const char *json, registry_entry_list *out) {
    return parse_entries(json, "results", out);
}

bool registry_parse_error(const char *json, char *code, size_t code_size, char *message,
                          size_t message_size) {
    json_document *doc = json_parse(json);
    if (doc == NULL)
        return false;

    json_value root = json_root(doc);
    bool ok = copy_string(json_get(root, "error"), code, code_size);
    if (ok)
        copy_optional(json_get(root, "message"), message, message_size);

    json_free(doc);
    return ok;
}

/* --- naming --- */

const char *registry_host_target(void) { return HOST_TARGET; }

bool registry_name_is_simple(const char *name) {
    if (name == NULL || name[0] == '\0')
        return false;
    /* Leading punctuation is what makes "." and ".." possible. */
    if (!isalnum((unsigned char)name[0]))
        return false;
    for (const char *c = name; *c != '\0'; c++) {
        if (isalnum((unsigned char)*c) || *c == '.' || *c == '-' || *c == '_' || *c == '+')
            continue;
        return false;
    }
    return strlen(name) < REGISTRY_NAME_MAX;
}

/* --- asking the registry --- */

bool registry_base_url(char *out, size_t out_size) {
    char stored[REGISTRY_URL_MAX];
    const char *base = getenv(REGISTRY_URL_ENV);

    if (base == NULL || base[0] == '\0') {
        if (preference_registry_get(stored, sizeof stored) && stored[0] != '\0')
            base = stored;
        else
            base = REGISTRY_DEFAULT_URL;
    }

    if (!fs_format_path(out, out_size, "%s", base))
        return false;

    /* One trailing slash decides whether every composed URL doubles it. */
    size_t length = strlen(out);
    while (length > 0 && out[length - 1] == '/')
        out[--length] = '\0';
    return length > 0;
}

/* Where an index is kept between runs. It is cached state rather than a
   download: what lands in the downloads directory is on its way to becoming an
   installed toolchain, and this never is. */
static bool cache_file(const char *filename, char *out, size_t out_size) {
    char directory[PICKUP_PATHS_MAX];
    if (!paths_cache(directory, sizeof directory))
        return false;
    return fs_format_path(out, out_size, "%s/%s", directory, filename);
}

static bool cache_is_fresh(const char *path) {
    int64_t written_ns;
    if (!fs_mtime_ns(path, &written_ns))
        return false;
    long long age = (long long)time(NULL) - (long long)(written_ns / 1000000000);
    return age >= 0 && age < REGISTRY_CACHE_TTL_SECONDS;
}

/*
 * Fetch `url` into `path` and hand back its text, using what is already there
 * when it is recent enough.
 *
 * The caller owns the result. A `path` of NULL is a fetch that is never reused.
 */
static char *fetch_json(const char *url, const char *path, bool cached, bool refresh,
                        http_tick tick, void *context) {
    if (refresh && cached)
        (void)remove(path);

    if (!cached || !cache_is_fresh(path)) {
        char directory[PICKUP_PATHS_MAX];
        if (!paths_cache(directory, sizeof directory) || !fs_make_dirs(directory))
            return NULL;
        if (!http_download_watched(url, path, tick, context))
            return NULL;
    }

    return fs_read_file(path);
}

bool registry_fetch_catalogue(registry_kind kind, bool refresh, registry_entry_list *out) {
    return registry_fetch_catalogue_watched(kind, refresh, NULL, NULL, out);
}

bool registry_fetch_catalogue_watched(registry_kind kind, bool refresh, http_tick tick,
                                      void *context, registry_entry_list *out) {
    registry_entry_list_init(out);

    const char *segment = registry_kind_path(kind);
    char base[REGISTRY_URL_MAX];
    char url[REGISTRY_URL_MAX];
    char filename[REGISTRY_NAME_MAX * 2];
    char path[PICKUP_PATHS_MAX];

    if (!registry_base_url(base, sizeof base) ||
        !fs_format_path(url, sizeof url, "%s/%s/%s", base, API_PREFIX, segment) ||
        !fs_format_path(filename, sizeof filename, CATALOGUE_CACHE_FORMAT, segment) ||
        !cache_file(filename, path, sizeof path))
        return false;

    char *text = fetch_json(url, path, true, refresh, tick, context);
    if (text == NULL)
        return false;

    bool ok = registry_parse_catalogue(text, out);
    free(text);
    /* A cached answer that no longer parses is worthless; drop it so the next
       run fetches a fresh one instead of failing again. */
    if (!ok)
        (void)remove(path);
    return ok;
}

bool registry_fetch_releases(registry_kind kind, const char *name, const char *version_filter,
                             const char *target, bool refresh, registry_artifact_list *out) {
    return registry_fetch_releases_watched(kind, name, version_filter, target, refresh, NULL, NULL,
                                           out);
}

bool registry_fetch_releases_watched(registry_kind kind, const char *name,
                                     const char *version_filter, const char *target, bool refresh,
                                     http_tick tick, void *context, registry_artifact_list *out) {
    registry_artifact_list_init(out);
    if (!registry_name_is_simple(name))
        return false;

    const char *segment = registry_kind_path(kind);
    char base[REGISTRY_URL_MAX];
    char url[REGISTRY_URL_MAX];
    char filename[REGISTRY_NAME_MAX * 3];
    char path[PICKUP_PATHS_MAX];

    if (!registry_base_url(base, sizeof base) ||
        !fs_format_path(url, sizeof url, "%s/%s/%s/%s", base, API_PREFIX, segment, name) ||
        !fs_format_path(filename, sizeof filename, RELEASES_CACHE_FORMAT, segment, name) ||
        !cache_file(filename, path, sizeof path))
        return false;

    char *text = fetch_json(url, path, true, refresh, tick, context);
    if (text == NULL)
        return false;

    bool ok = registry_parse_releases(text, version_filter, target, out);
    free(text);
    if (!ok)
        (void)remove(path);
    return ok;
}

bool registry_find(const char *name, bool refresh, registry_entry *out) {
    if (!registry_name_is_simple(name))
        return false;

    static const registry_kind order[] = {
        registry_kind_toolchain,
        registry_kind_tool,
        registry_kind_package,
    };

    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        registry_entry_list list;
        if (!registry_fetch_catalogue(order[i], refresh, &list))
            continue;

        bool found = false;
        for (size_t j = 0; j < list.count && !found; j++) {
            if (strcmp(list.items[j].name, name) == 0) {
                *out = list.items[j];
                found = true;
            }
        }
        registry_entry_list_free(&list);
        if (found)
            return true;
    }
    return false;
}

bool registry_search_watched(const char *query, http_tick tick, void *context,
                             registry_entry_list *out) {
    registry_entry_list_init(out);
    /* The query goes into a URL unescaped, so it is held to what needs no
       escaping. A search is a convenience; refusing an odd one costs nothing. */
    if (!registry_name_is_simple(query))
        return false;

    char base[REGISTRY_URL_MAX];
    char url[REGISTRY_URL_MAX];
    char path[PICKUP_PATHS_MAX];

    if (!registry_base_url(base, sizeof base) ||
        !fs_format_path(url, sizeof url, "%s/%s/search?q=%s", base, API_PREFIX, query) ||
        !cache_file(SEARCH_CACHE_FILE, path, sizeof path))
        return false;

    char *text = fetch_json(url, path, false, false, tick, context);
    if (text == NULL)
        return false;

    bool ok = registry_parse_search(text, out);
    free(text);
    return ok;
}
