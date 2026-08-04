#include <pickup/services/paths_service.h>

#include <pickup/services/fs_service.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Subdirectories of the pickup home. */
#define TOOLCHAINS_DIRNAME "toolchains"
#define TOOLS_DIRNAME      "tools"
#define DOWNLOADS_DIRNAME  "downloads"
#define CACHE_DIRNAME      "cache"

bool paths_home(char *out, size_t out_size) {
    const char *override = getenv(PICKUP_HOME_ENV);
    if (override != NULL && override[0] != '\0')
        return fs_format_path(out, out_size, "%s", override);

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        return false;
    return fs_format_path(out, out_size, "%s/%s", home, PICKUP_HOME_DIRNAME);
}

/* Compose `subdirectory` under the pickup home. */
static bool home_subdir(const char *subdirectory, char *out, size_t out_size) {
    char home[PICKUP_PATHS_MAX];
    if (!paths_home(home, sizeof home))
        return false;
    return fs_format_path(out, out_size, "%s/%s", home, subdirectory);
}

bool paths_toolchains(char *out, size_t out_size) {
    return home_subdir(TOOLCHAINS_DIRNAME, out, out_size);
}

bool paths_tools(char *out, size_t out_size) {
    return home_subdir(TOOLS_DIRNAME, out, out_size);
}

bool paths_cache(char *out, size_t out_size) {
    return home_subdir(CACHE_DIRNAME, out, out_size);
}

bool paths_downloads(char *out, size_t out_size) {
    return home_subdir(DOWNLOADS_DIRNAME, out, out_size);
}

bool paths_toolchain_dir(const char *vendor, const char *version,
                         const char *target, char *out, size_t out_size) {
    char toolchains[PICKUP_PATHS_MAX];
    if (!paths_toolchains(toolchains, sizeof toolchains))
        return false;
    return fs_format_path(out, out_size, "%s/%s-%s-%s",
                          toolchains, vendor, version, target);
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
    const char *root = realpath(toolchains, resolved) != NULL ? resolved : toolchains;

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
