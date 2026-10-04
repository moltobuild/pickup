#ifndef PICKUP_TESTS_USER_DIRS_FIXTURE_H
#define PICKUP_TESTS_USER_DIRS_FIXTURE_H

/*
 * Every variable that decides where Pickup writes, saved and put back.
 *
 * The suites that test the platform directories have to unset PICKUP_HOME —
 * that is the default users get — and once it is unset, HOME, the XDG
 * variables and APPDATA are what stands between the test and the real home
 * directory of whoever runs it. So all of them are pointed into a temporary
 * directory, and none is left the way the test found it by accident.
 */

#include <moltest.h>

#include <pickup/services/fs_service.h>
#include <pickup/services/paths_service.h>

#include <stdio.h>
#include <stdlib.h>

static const char *const user_dirs_variables[] = {
    PICKUP_HOME_ENV, "HOME",    "XDG_CONFIG_HOME", "XDG_DATA_HOME",
    "XDG_CACHE_HOME", "APPDATA", "USERPROFILE",
};
#define USER_DIRS_VARIABLE_COUNT (sizeof user_dirs_variables / sizeof user_dirs_variables[0])

typedef struct {
    char root[PICKUP_PATHS_MAX];
    char saved[USER_DIRS_VARIABLE_COUNT][PICKUP_PATHS_MAX];
    bool had[USER_DIRS_VARIABLE_COUNT];
} user_dirs_fixture;

/* Set `name` to `root` followed by `suffix`, or unset it when `suffix` is NULL. */
static inline bool user_dirs_point(const user_dirs_fixture *fixture, const char *name,
                                   const char *suffix) {
    if (suffix == NULL)
        return unsetenv(name) == 0;
    char value[PICKUP_PATHS_MAX];
    snprintf(value, sizeof value, "%s%s", fixture->root, suffix);
    return setenv(name, value, 1) == 0;
}

/*
 * A temporary root holding a home directory, the three XDG bases and an
 * APPDATA, all set, and PICKUP_HOME unset: the machine of a user who never
 * relocated anything.
 */
static inline bool user_dirs_setup(user_dirs_fixture *fixture, const char *prefix) {
    for (size_t i = 0; i < USER_DIRS_VARIABLE_COUNT; i++) {
        const char *value = getenv(user_dirs_variables[i]);
        fixture->had[i] = value != NULL;
        snprintf(fixture->saved[i], sizeof fixture->saved[i], "%s", value != NULL ? value : "");
    }
    if (!moltest_temp_dir(prefix, fixture->root, sizeof fixture->root))
        return false;
    return user_dirs_point(fixture, PICKUP_HOME_ENV, NULL) &&
           user_dirs_point(fixture, "HOME", "/home") &&
           user_dirs_point(fixture, "USERPROFILE", "/home") &&
           user_dirs_point(fixture, "XDG_CONFIG_HOME", "/xdg/config") &&
           user_dirs_point(fixture, "XDG_DATA_HOME", "/xdg/data") &&
           user_dirs_point(fixture, "XDG_CACHE_HOME", "/xdg/cache") &&
           user_dirs_point(fixture, "APPDATA", "/appdata");
}

static inline void user_dirs_teardown(user_dirs_fixture *fixture) {
    for (size_t i = 0; i < USER_DIRS_VARIABLE_COUNT; i++) {
        if (fixture->had[i])
            (void)setenv(user_dirs_variables[i], fixture->saved[i], 1);
        else
            (void)unsetenv(user_dirs_variables[i]);
    }
    (void)fs_remove_tree(fixture->root);
}

#endif /* PICKUP_TESTS_USER_DIRS_FIXTURE_H */
