#include <pickup/services/conda_service.h>

#include <pickup/services/archive_service.h>
#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/util/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Where a package's metadata is unpacked while it is being read. It is not
   part of what gets installed. */
#define INFO_TEMPLATE "/tmp/pickup_info_XXXXXX"

/* The metadata file listing every path in the package, and the fields of it
   that matter here. */
#define PATHS_FILE        "info/paths.json"
#define FIELD_PATHS       "paths"
#define FIELD_PATH        "_path"
#define FIELD_PLACEHOLDER "prefix_placeholder"
#define FIELD_FILE_MODE   "file_mode"
#define MODE_BINARY       "binary"

/* Find every occurrence of `needle` in `size` bytes and hand each offset to
   the caller. Written over bytes rather than strings because a binary holds
   NUL bytes, and strstr would stop at the first one. */
static const unsigned char *find_bytes(const unsigned char *haystack, size_t size,
                                       const char *needle, size_t needle_size) {
    if (needle_size == 0 || size < needle_size)
        return NULL;
    for (size_t i = 0; i + needle_size <= size; i++) {
        if (memcmp(haystack + i, needle, needle_size) == 0)
            return haystack + i;
    }
    return NULL;
}

/*
 * Rewrite inside a binary, keeping its length.
 *
 * The file must not change size: an ELF is full of offsets into itself, and
 * everything after a shortened path would move. So the replacement is written
 * in place and the difference is made up with NUL bytes.
 *
 * Where those bytes go is the whole subtlety. They belong at the end of the
 * C string the prefix sits in, not immediately after the prefix. A linker
 * carries its own script embedded as text — SEARCH_DIR("=<prefix>/lib") — and
 * padding straight after the prefix cuts that string in half, leaving a quote
 * that is never closed and a script the linker refuses to parse. Moving the
 * remainder of the string down and padding behind it keeps the whole path
 * intact, which is what the string was for.
 */
static bool rewrite_binary(unsigned char *data, size_t size, const char *placeholder,
                           const char *prefix) {
    size_t placeholder_size = strlen(placeholder);
    size_t prefix_size = strlen(prefix);
    if (prefix_size > placeholder_size)
        return false; /* nowhere to put it without moving everything after */

    unsigned char *cursor = data;
    unsigned char *end = data + size;
    for (;;) {
        unsigned char *found = (unsigned char *)find_bytes(cursor, (size_t)(end - cursor),
                                                           placeholder, placeholder_size);
        if (found == NULL)
            break;

        /* The C string this occurrence belongs to runs to the next NUL. */
        unsigned char *terminator = found + placeholder_size;
        while (terminator < end && *terminator != '\0')
            terminator++;

        /* What follows the prefix within that string, kept whole. */
        unsigned char *tail = found + placeholder_size;
        size_t tail_size = (size_t)(terminator - tail);
        size_t shrink = placeholder_size - prefix_size;

        memcpy(found, prefix, prefix_size);
        memmove(found + prefix_size, tail, tail_size);
        memset(found + prefix_size + tail_size, 0, shrink);

        cursor = found + prefix_size + tail_size;
        if (cursor >= end)
            break;
    }
    return true;
}

/* Rewrite inside text, where the file is free to change length. */
static bool rewrite_text(const char *path, const unsigned char *data, size_t size,
                         const char *placeholder, const char *prefix) {
    size_t placeholder_size = strlen(placeholder);
    size_t prefix_size = strlen(prefix);

    /* The replacement may be longer, so the room is worked out from the worst
       case: every byte of the file being the start of an occurrence. */
    size_t occurrences = 0;
    for (size_t i = 0; i + placeholder_size <= size; i++) {
        if (memcmp(data + i, placeholder, placeholder_size) == 0)
            occurrences++;
    }
    if (occurrences == 0)
        return true;

    size_t grown = size + occurrences * prefix_size + 1;
    unsigned char *out = malloc(grown);
    if (out == NULL)
        return false;

    size_t read = 0, written = 0;
    while (read < size) {
        if (read + placeholder_size <= size
            && memcmp(data + read, placeholder, placeholder_size) == 0) {
            memcpy(out + written, prefix, prefix_size);
            written += prefix_size;
            read += placeholder_size;
            continue;
        }
        out[written++] = data[read++];
    }

    bool ok = fs_write_bytes(path, out, written);
    free(out);
    return ok;
}

bool conda_rewrite_prefix(const char *path, const char *placeholder,
                          const char *prefix, bool binary) {
    if (path == NULL || placeholder == NULL || prefix == NULL
        || placeholder[0] == '\0')
        return false;

    size_t size = 0;
    unsigned char *data = fs_read_bytes(path, &size);
    if (data == NULL)
        return false;

    bool ok;
    if (binary) {
        ok = rewrite_binary(data, size, placeholder, prefix)
            && fs_write_bytes(path, data, size);
    } else {
        ok = rewrite_text(path, data, size, placeholder, prefix);
    }

    free(data);
    return ok;
}

bool conda_rewrite_all(const char *paths_json, const char *prefix, size_t *rewritten) {
    if (rewritten != NULL)
        *rewritten = 0;

    json_document *doc = json_parse(paths_json);
    if (doc == NULL)
        return false;

    json_value paths = json_get(json_root(doc), FIELD_PATHS);
    bool ok = true;
    for (size_t i = 0; ok && i < json_count(paths); i++) {
        json_value entry = json_at(paths, i);

        /* Only the files that recorded where they were built. Most do not, and
           reading every file to search for a prefix it never had would cost a
           pass over the whole installation. */
        const char *placeholder = json_string(json_get(entry, FIELD_PLACEHOLDER));
        const char *relative = json_string(json_get(entry, FIELD_PATH));
        if (placeholder == NULL || placeholder[0] == '\0' || relative == NULL)
            continue;

        const char *mode = json_string(json_get(entry, FIELD_FILE_MODE));
        bool binary = mode != NULL && strcmp(mode, MODE_BINARY) == 0;

        char full[PICKUP_PATHS_MAX];
        if (!fs_format_path(full, sizeof full, "%s/%s", prefix, relative))
            continue;
        /* A file the package lists and the extraction did not produce is not a
           reason to fail: the selection of what to unpack is Pickup's own. */
        if (!fs_path_exists(full))
            continue;

        ok = conda_rewrite_prefix(full, placeholder, prefix, binary);
        if (ok && rewritten != NULL)
            (*rewritten)++;
    }

    json_free(doc);
    return ok;
}

bool conda_install_package(const char *package, const char *prefix) {
    if (!archive_extract_conda(package, prefix))
        return false;

    /* The metadata is read from a directory of its own, because it describes
       the installation rather than belonging to it. */
    char info_dir[] = INFO_TEMPLATE;
    if (mkdtemp(info_dir) == NULL)
        return false;

    bool ok = archive_extract_conda_info(package, info_dir);
    if (ok) {
        char paths_path[PICKUP_PATHS_MAX];
        ok = fs_format_path(paths_path, sizeof paths_path, "%s/%s",
                            info_dir, PATHS_FILE);
        if (ok) {
            char *text = fs_read_file(paths_path);
            /* A package with no paths.json carries nothing to rewrite, which
               is true of the ones that are pure data. */
            if (text != NULL) {
                ok = conda_rewrite_all(text, prefix, NULL);
                free(text);
            }
        }
    }

    (void)fs_remove_tree(info_dir);
    return ok;
}
