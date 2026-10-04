#include <pickup/services/paths_service.h>

#include <pickup/services/fs_service.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Subdirectories of the data and cache roots, and of a relocated home. */
#define TOOLCHAINS_DIRNAME "toolchains"
#define TOOLS_DIRNAME "tools"
#define DOWNLOADS_DIRNAME "downloads"
#define CACHE_DIRNAME "cache"

/* The user's home directory, as the legacy location was always found. */
static const char *user_home(void) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    /* Windows names it differently, and only a shell that brings its own
       environment — MSYS2, git-bash — sets HOME at all. */
    if (home == NULL || home[0] == '\0')
        home = getenv("USERPROFILE");
#endif
    return home != NULL && home[0] != '\0' ? home : NULL;
}

/* `base`/pickup, without doubling a separator `base` already ends with: the
   toolchains directory is compared by prefix, and `a//pickup` is not a prefix
   of `a/pickup/...`. */
static bool pickup_under(const char *base, char *out, size_t out_size) {
    size_t length = strlen(base);
    while (length > 1 && (base[length - 1] == '/' || base[length - 1] == '\\'))
        length--;
    return fs_format_path(out, out_size, "%.*s/%s", (int)length, base, PICKUP_DIRNAME);
}

/*
 * The three roots, decided by the platform: the one block in this file that
 * differs between them.
 */
#ifdef _WIN32

/*
 * %APPDATA%\pickup, which is what was asked for.
 *
 * APPDATA roams: on a domain account it is copied to a server at logoff, and a
 * toolchain is hundreds of megabytes that are worthless on another machine.
 * %LOCALAPPDATA% is where data of that kind belongs. Each root below is chosen
 * by exactly one line, so moving data and cache there is changing "APPDATA" to
 * "LOCALAPPDATA" in data_root and cache_root — and leaving config_root, the one
 * thing that is worth roaming, where it is.
 */
static bool appdata_root(const char *variable, char *out, size_t out_size) {
    const char *base = getenv(variable);
    if (base == NULL || base[0] == '\0')
        return false;
    return pickup_under(base, out, out_size);
}

static bool config_root(char *out, size_t out_size) {
    return appdata_root("APPDATA", out, out_size);
}

static bool data_root(char *out, size_t out_size) { return appdata_root("APPDATA", out, out_size); }

/* Inside the same directory as the data, and apart from it: deleting the cache
   must never be able to take an installed toolchain with it. */
static bool cache_root(char *out, size_t out_size) {
    char root[PICKUP_PATHS_MAX];
    return appdata_root("APPDATA", root, sizeof root) &&
           fs_format_path(out, out_size, "%s/%s", root, CACHE_DIRNAME);
}

#else

/*
 * $variable/pickup, or ~/fallback/pickup.
 *
 * The XDG Base Directory specification says a value that is empty or not an
 * absolute path is invalid and must be ignored, and it is ignored rather than
 * reported: honouring a relative one would scatter toolchains across whatever
 * directories pickup happened to be run from.
 */
static bool xdg_root(const char *variable, const char *fallback, char *out, size_t out_size) {
    const char *base = getenv(variable);
    if (base != NULL && base[0] == '/')
        return pickup_under(base, out, out_size);

    const char *home = user_home();
    if (home == NULL)
        return false;
    char joined[PICKUP_PATHS_MAX];
    return fs_format_path(joined, sizeof joined, "%s/%s", home, fallback) &&
           pickup_under(joined, out, out_size);
}

static bool config_root(char *out, size_t out_size) {
    return xdg_root("XDG_CONFIG_HOME", ".config", out, out_size);
}

static bool data_root(char *out, size_t out_size) {
    return xdg_root("XDG_DATA_HOME", ".local/share", out, out_size);
}

static bool cache_root(char *out, size_t out_size) {
    return xdg_root("XDG_CACHE_HOME", ".cache", out, out_size);
}

#endif

bool paths_relocated(void) {
    const char *override = getenv(PICKUP_HOME_ENV);
    return override != NULL && override[0] != '\0';
}

bool paths_legacy_home(char *out, size_t out_size) {
    const char *home = user_home();
    if (home == NULL)
        return false;
    return fs_format_path(out, out_size, "%s/%s", home, PICKUP_HOME_DIRNAME);
}

bool paths_home(char *out, size_t out_size) {
    if (paths_relocated())
        return fs_format_path(out, out_size, "%s", getenv(PICKUP_HOME_ENV));
    return paths_legacy_home(out, out_size);
}

/* Compose `subdirectory` under the relocated home. */
static bool home_subdir(const char *subdirectory, char *out, size_t out_size) {
    char home[PICKUP_PATHS_MAX];
    if (!paths_home(home, sizeof home))
        return false;
    return fs_format_path(out, out_size, "%s/%s", home, subdirectory);
}

/* Compose `subdirectory` under the data root. */
static bool data_subdir(const char *subdirectory, char *out, size_t out_size) {
    char data[PICKUP_PATHS_MAX];
    if (!paths_data(data, sizeof data))
        return false;
    return fs_format_path(out, out_size, "%s/%s", data, subdirectory);
}

bool paths_config(char *out, size_t out_size) {
    return paths_relocated() ? paths_home(out, out_size) : config_root(out, out_size);
}

bool paths_data(char *out, size_t out_size) {
    return paths_relocated() ? paths_home(out, out_size) : data_root(out, out_size);
}

bool paths_toolchains(char *out, size_t out_size) {
    return data_subdir(TOOLCHAINS_DIRNAME, out, out_size);
}

bool paths_tools(char *out, size_t out_size) { return data_subdir(TOOLS_DIRNAME, out, out_size); }

bool paths_cache(char *out, size_t out_size) {
    return paths_relocated() ? home_subdir(CACHE_DIRNAME, out, out_size)
                             : cache_root(out, out_size);
}

/* Inside the cache when nothing is relocated: an archive in flight is the most
   disposable thing Pickup writes. A relocated home keeps the sibling it always
   had. */
bool paths_downloads(char *out, size_t out_size) {
    if (paths_relocated())
        return home_subdir(DOWNLOADS_DIRNAME, out, out_size);
    char cache[PICKUP_PATHS_MAX];
    return cache_root(cache, sizeof cache) &&
           fs_format_path(out, out_size, "%s/%s", cache, DOWNLOADS_DIRNAME);
}

bool paths_toolchain_dir(const char *vendor, const char *version, const char *target, char *out,
                         size_t out_size) {
    char toolchains[PICKUP_PATHS_MAX];
    if (!paths_toolchains(toolchains, sizeof toolchains))
        return false;
    return fs_format_path(out, out_size, "%s/%s-%s-%s", toolchains, vendor, version, target);
}

bool paths_owning_toolchain(const char *path, char *out, size_t out_size) {
    if (path == NULL || path[0] == '\0')
        return false;

    char toolchains[PICKUP_PATHS_MAX];
    if (!paths_toolchains(toolchains, sizeof toolchains))
        return false;

    /* Canonicalised when it exists, because the home may be reached through a
       symlink while the compiler path was resolved: /home -> /var/home would
       otherwise make everything installed look like a system compiler. */
    char resolved[PATH_MAX];
    const char *root = fs_real_path(toolchains, resolved, sizeof resolved) ? resolved : toolchains;

    size_t length = strlen(root);
    if (strncmp(path, root, length) != 0 || path[length] != '/')
        return false;

    /* The first component below the root, and nothing deeper: the binary is
       several levels down, and what gets removed is the whole toolchain. */
    const char *name = path + length + 1;
    const char *end = strchr(name, '/');
    size_t name_length = end != NULL ? (size_t)(end - name) : strlen(name);
    if (name_length == 0)
        return false;

    return fs_format_path(out, out_size, "%s/%.*s", root, (int)name_length, name);
}
