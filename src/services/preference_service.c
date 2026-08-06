#include <pickup/services/preference_service.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The file, in the pickup home rather than in the cache: see the header. */
#define CONFIG_FILENAME "config.toml"

/* The keys this version understands. Only the first is ever written. */
#define KEY_DEFAULT  "default"
#define KEY_REGISTRY "registry"

/* Room for the whole file. It holds a handful of short keys; anything larger
   than this is not a configuration file Pickup wrote. */
#define CONFIG_MAX 8192

/* How a key is stored, and the only shape read back. */
#define KEY_FORMAT "%s = \"%s\"\n"

static bool config_path(char *out, size_t out_size) {
    char home[PICKUP_PATHS_MAX];
    if (!paths_home(home, sizeof home))
        return false;
    return fs_format_path(out, out_size, "%s/%s", home, CONFIG_FILENAME);
}

/* Past the spaces at the front of a line. */
static const char *skip_spaces(const char *cursor) {
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    return cursor;
}

/* The start of the line after this one, or NULL at the end of the text. */
static const char *next_line(const char *line) {
    const char *newline = strchr(line, '\n');
    return newline != NULL ? newline + 1 : NULL;
}

/* True if `line` assigns `key`. Used both to read a value and to drop the old
   assignment before writing a new one, so the two can never disagree about
   which line is which. */
static bool line_assigns(const char *line, const char *key) {
    const char *cursor = skip_spaces(line);
    size_t length = strlen(key);
    if (strncmp(cursor, key, length) != 0)
        return false;

    cursor = skip_spaces(cursor + length);
    return *cursor == '=';
}

/* The quoted value out of `key = "value"`. False if the line is not in that
   shape: a value Pickup did not write is not one it can act on. */
static bool read_assigned(const char *line, const char *key,
                          char *out, size_t out_size) {
    const char *cursor = skip_spaces(skip_spaces(line) + strlen(key));
    if (*cursor != '=')
        return false;
    cursor = skip_spaces(cursor + 1);
    if (*cursor != '"')
        return false;
    cursor++;

    const char *end = strchr(cursor, '"');
    if (end == NULL)
        return false;

    size_t length = (size_t)(end - cursor);
    if (length == 0 || length >= out_size)
        return false;
    memcpy(out, cursor, length);
    out[length] = '\0';
    return true;
}

/* Read the value of `key`. False when the file is absent or says nothing about
   it, which are the same answer to a caller: no preference was recorded. */
static bool read_key(const char *key, char *out, size_t out_size) {
    char path[PICKUP_PATHS_MAX];
    if (!config_path(path, sizeof path))
        return false;

    char *text = fs_read_file(path);
    if (text == NULL)
        return false;

    bool found = false;
    for (const char *line = text; line != NULL && !found; line = next_line(line)) {
        if (line_assigns(line, key))
            found = read_assigned(line, key, out, out_size);
    }
    free(text);
    return found;
}

/*
 * Copy `text` into `out` without the line that assigns `key`.
 *
 * Every other line is carried over byte for byte, comments and unknown keys
 * included. A configuration file is something a person may have edited, and
 * rewriting one by regenerating only the parts this version understands would
 * quietly delete the rest.
 */
static bool copy_without(const char *text, const char *key,
                         char *out, size_t out_size) {
    size_t used = 0;
    for (const char *line = text; line != NULL; line = next_line(line)) {
        const char *after = next_line(line);
        size_t length = after != NULL ? (size_t)(after - line) : strlen(line);
        if (length == 0 || line_assigns(line, key))
            continue;
        if (used + length >= out_size)
            return false;
        memcpy(out + used, line, length);
        used += length;
    }

    /* A last line that ended without a newline would otherwise run into the
       assignment appended after it. */
    if (used > 0 && out[used - 1] != '\n') {
        if (used + 1 >= out_size)
            return false;
        out[used++] = '\n';
    }
    out[used] = '\0';
    return true;
}

/* Write `key = "value"`, replacing any assignment already there. A NULL
   `value` removes the key instead. */
static bool write_key(const char *key, const char *value) {
    char home[PICKUP_PATHS_MAX];
    char path[PICKUP_PATHS_MAX];
    if (!paths_home(home, sizeof home) || !config_path(path, sizeof path))
        return false;

    char content[CONFIG_MAX] = "";
    char *existing = fs_read_file(path);
    if (existing != NULL) {
        bool copied = copy_without(existing, key, content, sizeof content);
        free(existing);
        if (!copied)
            return false;
    }

    if (value != NULL) {
        size_t used = strlen(content);
        int written = snprintf(content + used, sizeof content - used,
                               KEY_FORMAT, key, value);
        if (written < 0 || (size_t)written >= sizeof content - used)
            return false;
    }

    /* Nothing left to say: the file goes rather than staying behind empty, so
       clearing the only preference leaves the home as it was found. */
    if (content[0] == '\0') {
        (void)remove(path);
        return true;
    }

    /* The home may not exist yet: setting a default is a reason to create it,
       the same way installing something is. */
    if (!fs_make_dirs(home))
        return false;
    return fs_write_file(path, content);
}

bool preference_default_get(char *out, size_t out_size) {
    return read_key(KEY_DEFAULT, out, out_size);
}

bool preference_default_set(const char *id) {
    if (id == NULL || id[0] == '\0')
        return false;
    return write_key(KEY_DEFAULT, id);
}

bool preference_default_clear(void) {
    return write_key(KEY_DEFAULT, NULL);
}

bool preference_registry_get(char *out, size_t out_size) {
    return read_key(KEY_REGISTRY, out, out_size);
}
