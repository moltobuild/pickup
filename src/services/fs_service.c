#include <pickup/services/fs_service.h>

#include <pickup/services/paths_service.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <ctype.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

/*
 * The platform, in one block, because every function below this line is one
 * implementation (RFC-0017). A second `#ifdef` further down would be the first
 * step towards two versions of this file pretending to be one.
 */

/* Windows has no mode argument: a directory's permissions are inherited from
   its parent rather than stated at creation. */
#ifdef _WIN32
#define make_one_dir(path) _mkdir(path)
#else
#define make_one_dir(path) mkdir((path), 0755)
#endif

/*
 * `stat` that does not follow a symlink.
 *
 * On Windows it does follow one, because there is no `lstat` and the reparse
 * points that stand in for symlinks need a different API entirely. The two
 * callers can live with it: one answers "is this a directory, without being
 * fooled by a link to one", and the other adds up a tree without walking into
 * a link that points back at it. Both are about not looping, and Windows
 * requires a privilege to create the links that would cause the loop.
 *
 * Stated here rather than silently: it is a real difference in behaviour, and
 * the day something creates those links on Windows this is where the answer
 * has to change.
 */
static int stat_no_follow(const char *path, struct stat *out) {
#ifdef _WIN32
    return stat(path, out);
#else
    return lstat(path, out);
#endif
}

/* Nanoseconds in one second, for composing a timestamp. */
#define NANOS_PER_SECOND 1000000000LL

/* Modification time in nanoseconds.
 *
 * Windows keeps whole seconds in `struct stat`, so the answer there is a
 * second multiplied out rather than a second measured. What depends on this is
 * freshness: two writes inside one second look simultaneous, and a rebuild
 * that would have been triggered by nanoseconds is not. It is the honest
 * ceiling of the interface, not a rounding this code chose. */
static int64_t stat_mtime_ns(const struct stat *info) {
#ifdef _WIN32
    return (int64_t)info->st_mtime * NANOS_PER_SECOND;
#else
    return (int64_t)info->st_mtim.tv_sec * NANOS_PER_SECOND + (int64_t)info->st_mtim.tv_nsec;
#endif
}

bool fs_path_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

bool fs_make_dir(const char *path) {
    if (make_one_dir(path) == 0)
        return true;
    /* Treat an already existing directory as success. */
    struct stat info;
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
        return true;
    return false;
}

bool fs_write_file(const char *path, const char *content) {
    /* Binary, because text mode on Windows turns every `\n` into `\r\n` on the
       way out while `fs_read_file` opens "rb" and reads them back verbatim.
       Write, read, write, and a file grows a `\r` per line per round trip.
       Nothing here wants a platform's idea of a line: a config file pickup
       writes has to be the same bytes wherever it was written, which is the
       same argument RFC-0017 makes about the separator. */
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;
    size_t length = strlen(content);
    size_t written = fwrite(content, 1, length, file);
    int closed = fclose(file);
    return written == length && closed == 0;
}

char *fs_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[read] = '\0';
    return buffer;
}

unsigned char *fs_read_bytes(const char *path, size_t *size) {
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    /* One spare byte so that an empty file still yields a pointer, and a
       caller may treat the result as a string when it happens to be one. */
    unsigned char *buffer = malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(buffer);
        return NULL;
    }
    buffer[read] = '\0';
    *size = read;
    return buffer;
}

bool fs_write_bytes(const char *path, const unsigned char *data, size_t size) {
    /* Read first, so the mode can be put back: rewriting a compiler in place
       must not leave it unexecutable. */
    struct stat before;
    bool had_mode = stat(path, &before) == 0;

    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;
    size_t written = fwrite(data, 1, size, file);
    int closed = fclose(file);
    if (written != size || closed != 0)
        return false;

    if (had_mode)
        (void)chmod(path, before.st_mode & 07777);
    return true;
}

bool fs_is_dir(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool fs_is_dir_no_follow(const char *path) {
    struct stat info;
    return stat_no_follow(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool fs_format_path(char *out, size_t size, const char *format, ...) {
    if (size == 0)
        return false;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out, size, format, args);
    va_end(args);
    return written >= 0 && (size_t)written < size;
}

/* Where the first component that can actually be made begins.

   A leading `/` is the root and is already there. On Windows the root is a
   volume — `C:` — and asking to create it fails, which made this function
   unable to take an absolute Windows path at all. It went unnoticed because
   such a path used to arrive with backslashes in it, so the loop below found
   no `/` to split on before the last component and never tried. */
static size_t first_creatable(const char *path) {
    size_t at = 0;
#ifdef _WIN32
    if (path[0] != '\0' && path[1] == ':')
        at = 2;
#endif
    while (path[at] == '/')
        at++;
    return at;
}

bool fs_make_dirs(const char *path) {
    char buffer[4096];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof buffer)
        return false;
    memcpy(buffer, path, length + 1);
    /* Create each intermediate component in turn. */
    for (size_t i = first_creatable(path) + 1; i < length; i++) {
        if (buffer[i] != '/')
            continue;
        buffer[i] = '\0';
        if (!fs_make_dir(buffer))
            return false;
        buffer[i] = '/';
    }
    return fs_make_dir(buffer);
}

/* Size of the buffer used to compose the path of an entry being deleted. */
#define REMOVE_PATH_SIZE 4096

bool fs_remove_tree(const char *path) {
    if (!fs_is_dir_no_follow(path)) {
        /* A file, a symlink, or nothing at all. */
        return remove(path) == 0 || !fs_path_exists(path);
    }
    DIR *dir = opendir(path);
    if (dir == NULL)
        return false;
    bool ok = true;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[REMOVE_PATH_SIZE];
        if (!fs_format_path(child, sizeof child, "%s/%s", path, entry->d_name)) {
            ok = false;
            continue;
        }
        if (!fs_remove_tree(child))
            ok = false;
    }
    closedir(dir);
    return rmdir(path) == 0 && ok;
}

bool fs_mtime_ns(const char *path, int64_t *out) {
    struct stat info;
    if (stat(path, &info) != 0)
        return false;
    *out = stat_mtime_ns(&info);
    return true;
}

bool fs_file_size(const char *path, long long *out) {
    struct stat info;
    if (stat(path, &info) != 0)
        return false;
    *out = (long long)info.st_size;
    return true;
}

bool fs_rename(const char *from, const char *to) { return rename(from, to) == 0; }

/* Add up a directory, entry by entry. Symlinks count as the link itself and are
   never followed, so a link pointing back into the tree cannot be counted twice
   or send this walking forever. */
static bool accumulate_tree(const char *path, long long *total) {
    struct stat info;
    if (stat_no_follow(path, &info) != 0)
        return false;

    if (!S_ISDIR(info.st_mode)) {
        *total += (long long)info.st_size;
        return true;
    }

    DIR *dir = opendir(path);
    if (dir == NULL)
        return false;

    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[REMOVE_PATH_SIZE];
        if (!fs_format_path(child, sizeof child, "%s/%s", path, entry->d_name))
            continue;
        (void)accumulate_tree(child, total);
    }
    closedir(dir);
    return true;
}

bool fs_tree_size(const char *path, long long *out) {
    *out = 0;
    if (!fs_path_exists(path))
        return true; /* nothing there yet is a size of zero, not a failure */
    return accumulate_tree(path, out);
}

bool fs_source_newer(const char *source, const char *target) {
    int64_t target_ns;
    if (!fs_mtime_ns(target, &target_ns))
        return true; /* missing target: rebuild */
    int64_t source_ns;
    if (!fs_mtime_ns(source, &source_ns))
        return true; /* missing source: fail safe and rebuild */
    return source_ns > target_ns;
}

bool fs_real_path(const char *path, char *out, size_t size) {
#ifdef _WIN32
    /* `_fullpath` is lexical where `realpath` is not: it resolves `.` and `..`
       and makes the path absolute without ever asking the filesystem, and so
       answers just as happily for a path that is not there. Callers read a
       true here as "this exists and here is its real name", which is what
       `realpath` means, so the check is what makes the two the same function.

       It does not follow reparse points, which is the same limitation
       stat_no_follow records above. */
    if (!fs_path_exists(path))
        return false;
    char *answer = _fullpath(NULL, path, 0);
#else
    char *answer = realpath(path, NULL);
#endif
    if (answer == NULL)
        return false;
    const int written = snprintf(out, size, "%s", answer);
    free(answer);
    if (written < 0 || (size_t)written >= size)
        return false;

#ifdef _WIN32
    /* And back to `/`. RFC-0017 makes it the internal separator everywhere and
       has nothing convert it; `_fullpath` is one of the few places the platform
       converts anyway, and it hands back `\`. Every caller that takes a path
       apart looks for `/` — `directory_of` in recipe.c is one, and a `\` there
       made an installed toolchain's own libc++ invisible, which cost fifteen
       tests that never mentioned a separator. Normalising here keeps the rule
       true at the one place the platform breaks it. */
    for (char *at = out; *at != '\0'; at++) {
        if (*at == '\\')
            *at = '/';
    }
#endif
    return true;
}

#ifdef _WIN32
/* Case-insensitively, because a filesystem that does not distinguish `GCC.EXE`
   from `gcc.exe` will hand back either. */
static bool ends_with_exe(const char *file, size_t length) {
    static const char suffix[] = ".exe";
    const size_t width = sizeof suffix - 1;
    if (length <= width)
        return false;
    const char *tail = file + length - width;
    for (size_t i = 0; i < width; i++) {
        if (tolower((unsigned char)tail[i]) != suffix[i])
            return false;
    }
    return true;
}
#endif

bool fs_executable_name(const char *file, char *out, size_t size) {
    const size_t length = strlen(file);
#ifdef _WIN32
    if (!ends_with_exe(file, length))
        return false;
    const size_t bare = length - (sizeof ".exe" - 1);
#else
    const size_t bare = length;
#endif
    if (bare >= size)
        return false;
    memcpy(out, file, bare);
    out[bare] = '\0';
    return true;
}

/* What separates one directory from the next in PATH. A colon is part of a
   drive letter on Windows, so the platforms cannot share one. */
#ifdef _WIN32
#define PATH_ENTRY_SEPARATOR ';'
#else
#define PATH_ENTRY_SEPARATOR ':'
#endif

bool fs_walk_path(const char *path_env, bool (*visit)(const char *directory, void *context),
                  void *context) {
    if (path_env == NULL)
        return true;

    for (const char *cursor = path_env; *cursor != '\0';) {
        const char *separator = strchr(cursor, PATH_ENTRY_SEPARATOR);
        const size_t length = separator != NULL ? (size_t)(separator - cursor) : strlen(cursor);

        if (length > 0 && length < PICKUP_PATHS_MAX) {
            char directory[PICKUP_PATHS_MAX];
            memcpy(directory, cursor, length);
            directory[length] = '\0';
            if (!visit(directory, context))
                return false;
        }
        if (separator == NULL)
            break;
        cursor = separator + 1;
    }
    return true;
}

/* A backslash ends a component on Windows; on POSIX it is an ordinary
   character that a filename may contain. */
static bool is_separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool fs_program_name(const char *path, char *out, size_t size) {
    const char *file = path;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (is_separator(*cursor))
            file = cursor + 1;
    }

    size_t length = strlen(file);
#ifdef _WIN32
    if (ends_with_exe(file, length))
        length -= sizeof ".exe" - 1;
#endif
    if (length >= size)
        return false;
    memcpy(out, file, length);
    out[length] = '\0';
    return true;
}

bool fs_executable_file(const char *name, char *out, size_t size) {
#ifdef _WIN32
    const int written = snprintf(out, size, "%s.exe", name);
#else
    const int written = snprintf(out, size, "%s", name);
#endif
    return written > 0 && (size_t)written < size;
}

/* Where the platform keeps files nobody intends to keep. */
static const char *temp_root(void) {
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (base == NULL)
        base = getenv("TMP");
    return base != NULL ? base : ".";
#else
    return "/tmp";
#endif
}

/* The pattern both temporaries start from, and how long it is. */
static bool temp_pattern(const char *prefix, char *out, size_t size, size_t *length) {
    const int written = snprintf(out, size, "%s/%s_XXXXXX", temp_root(), prefix);
    if (written < 0 || (size_t)written >= size)
        return false;
    *length = (size_t)written;
    return true;
}

bool fs_temp_file(const char *prefix, char *out, size_t size) {
    size_t length = 0;
    if (!temp_pattern(prefix, out, size, &length))
        return false;
#ifdef _WIN32
    if (_mktemp_s(out, length + 1) != 0)
        return false;
    FILE *file = fopen(out, "wb");
    if (file == NULL)
        return false;
    (void)fclose(file);
    return true;
#else
    const int fd = mkstemp(out);
    if (fd < 0)
        return false;
    (void)close(fd);
    return true;
#endif
}

bool fs_temp_program(const char *prefix, char *out, size_t size) {
    size_t written = 0;
    if (!temp_pattern(prefix, out, size, &written))
        return false;

#ifdef _WIN32
    /* `_mktemp_s` picks the name and creates nothing, so the file is made here
       — and `.exe` goes on after the name is picked, because the pattern it
       replaces has to be at the end. */
    if (_mktemp_s(out, written + 1) != 0)
        return false;
    if (written + sizeof ".exe" > size)
        return false;
    memcpy(out + written, ".exe", sizeof ".exe");
    FILE *file = fopen(out, "wb");
    if (file == NULL)
        return false;
    (void)fclose(file);
    return true;
#else
    const int fd = mkstemp(out);
    if (fd < 0)
        return false;
    (void)close(fd);
    return true;
#endif
}
