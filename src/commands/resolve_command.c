#include <pickup/commands/resolve_command.h>

#include <pickup/commands/probe_progress.h>
#include <pickup/detect/recipe.h>
#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Separator between feature ids in --require. */
#define FEATURE_SEPARATOR ','

/* Size of the buffer holding one feature id while it is parsed. */
#define FEATURE_ID_SIZE 64

/* Language names accepted on the command line. */
#define LANG_C_NAME    "c"
#define LANG_CXX_NAME  "c++"

/* Parse the language, defaulting to C. Returns false if it is not a language
   Pickup knows. */
static bool parse_lang(const char *name, capability_lang *out) {
    if (name == NULL || strcmp(name, LANG_C_NAME) == 0) {
        *out = lang_c;
        return true;
    }
    if (strcmp(name, LANG_CXX_NAME) == 0) {
        *out = lang_cxx;
        return true;
    }
    return false;
}

/* Add the features named in a comma-separated list to `required`. An unknown
   id is rejected rather than ignored: silently dropping a requirement would
   return a compiler that does not meet it. */
static bool add_named_features(const char *list, capability_set *required,
                               char *unknown, size_t unknown_size) {
    const char *cursor = list;
    while (*cursor != '\0') {
        const char *separator = strchr(cursor, FEATURE_SEPARATOR);
        size_t length = separator != NULL ? (size_t)(separator - cursor) : strlen(cursor);

        if (length > 0 && length < FEATURE_ID_SIZE) {
            char id[FEATURE_ID_SIZE];
            memcpy(id, cursor, length);
            id[length] = '\0';
            size_t index = capability_index(id);
            if (index == SIZE_MAX) {
                snprintf(unknown, unknown_size, "%s", id);
                return false;
            }
            capability_set_add(required, index);
        }

        if (separator == NULL)
            break;
        cursor = separator + 1;
    }
    return true;
}

/* The features a toolchain proved for `lang`. */
static capability_set features_of(const toolchain *chain, capability_lang lang) {
    return lang == lang_c ? chain->c_features : chain->cxx_features;
}

/* The binary to invoke for `lang`: the C++ driver when compiling C++. */
static const char *driver_for(const toolchain *chain, capability_lang lang) {
    return lang == lang_cxx ? chain->cxx_path : chain->path;
}

/* True if `chain` satisfies every part of the request.

   The standard and the features answer different questions. `--std` asks "can
   you be invoked in this mode?", which is about the flag. `--require` asks
   "does this actually work?", which is about the implementation. Conflating
   them is what makes a tool recommend a compiler that then fails to build. */
static bool satisfies(const toolchain *chain, const resolve_request *request,
                      capability_lang lang, capability_set required) {
    if (request->vendor != NULL
        && chain->vendor != toolchain_vendor_parse(request->vendor))
        return false;
    /* A C++ request needs a C++ driver, not merely a compiler that can parse
       C++: without one there is nothing to invoke. */
    if (lang == lang_cxx && chain->cxx_path[0] == '\0')
        return false;
    if (!capability_set_contains(features_of(chain, lang), required))
        return false;
    if (request->standard != NULL
        && !capability_accepts_standard(driver_for(chain, lang), lang, request->standard))
        return false;
    return true;
}

/* Print, for one rejected candidate, the first requirement it fails. Naming
   what is missing is the difference between a diagnosis and a shrug. */
static void print_missing(const toolchain *chain, capability_lang lang,
                          capability_set required, const char *standard) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);

    size_t count = 0;
    const capability *catalog = capability_catalog(&count);
    capability_set proven = features_of(chain, lang);

    fprintf(stderr, "  %-16s (%s) missing:", chain->name, version);
    if (lang == lang_cxx && chain->cxx_path[0] == '\0') {
        fprintf(stderr, " a C++ driver\n");
        return;
    }
    if (standard != NULL
        && !capability_accepts_standard(driver_for(chain, lang), lang, standard))
        fprintf(stderr, " -std=%s", standard);
    for (size_t i = 0; i < count; i++) {
        if (capability_set_has(required, i) && !capability_set_has(proven, i))
            fprintf(stderr, " %s", catalog[i].id);
    }
    fprintf(stderr, "\n");
}

/* Parse the standard library named on the command line. `stdlib_unknown` with
   a false return means the name was not one Pickup offers. */
static bool parse_stdlib(const char *name, cxx_stdlib *out) {
    if (name == NULL) {
        *out = stdlib_unknown;
        return true;
    }
    if (strcmp(name, "libstdc++") == 0) {
        *out = stdlib_libstdcxx;
        return true;
    }
    if (strcmp(name, "libc++") == 0) {
        *out = stdlib_libcxx;
        return true;
    }
    return false;
}

/*
 * The best toolchain that can be built with, not merely the best that matches.
 *
 * Matching the requirements is about what a compiler implements; a recipe is
 * about whether it produces a program that runs. They come apart in practice —
 * a Clang can implement all of C++20 and still fail on `#include <iostream>`
 * because the standard library it borrows is incomplete — and answering with
 * the first without checking the second is how a caller ends up reading
 * compiler errors instead of an answer.
 *
 * Candidates are tried highest version first, so the result is still
 * deterministic: the same machine yields the same answer every time.
 */
static const toolchain *select_buildable(const inventory *list,
                                         const resolve_request *request,
                                         capability_lang lang, capability_set required,
                                         cxx_stdlib wanted, link_recipe *recipe_out) {
    bool *tried = calloc(list->count > 0 ? list->count : 1, sizeof *tried);
    if (tried == NULL)
        return NULL;

    const toolchain *chosen = NULL;
    for (;;) {
        /* The best candidate not yet tried. */
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < list->count; i++) {
            if (tried[i] || !satisfies(&list->items[i], request, lang, required))
                continue;
            if (best == SIZE_MAX
                || toolchain_version_compare(list->items[i].version,
                                             list->items[best].version) > 0)
                best = i;
        }
        if (best == SIZE_MAX)
            break;
        tried[best] = true;

        /* The wanted library narrows the search inside the toolchain rather
           than filtering its answer: the recipe it would prefer may use the
           other one while a working recipe for this one exists further down
           its candidate list. */
        link_recipe recipe = recipe_discover_for(&list->items[best], lang, wanted);
        if (!recipe.usable)
            continue;

        *recipe_out = recipe;
        chosen = &list->items[best];
        break;
    }

    free(tried);
    return chosen;
}

static void print_text(const toolchain *chain, capability_lang lang) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);
    printf("%s %s %s (%s)\n", chain->name, toolchain_vendor_name(chain->vendor),
           version, lang == lang_cxx ? chain->cxx_path : chain->path);
}

/* Print a TOML array of strings on one line. */
static void print_string_array(const char *key, const char rows[][RECIPE_FLAG_MAX],
                               size_t count) {
    printf("%s = [", key);
    for (size_t i = 0; i < count; i++)
        printf("%s\"%s\"", i == 0 ? "" : ", ", rows[i]);
    printf("]\n");
}

static void print_dir_array(const char *key, const char rows[][PICKUP_PATHS_MAX],
                            size_t count) {
    printf("%s = [", key);
    for (size_t i = 0; i < count; i++)
        printf("%s\"%s\"", i == 0 ? "" : ", ", rows[i]);
    printf("]\n");
}

/*
 * How to build with what was resolved.
 *
 * A path alone is not enough to compile with, and a caller that has to work
 * the rest out ends up hard-coding flags for the machine it was written on —
 * the thing Pickup exists to prevent. Every field here was proven: the flags
 * are the ones that were watched producing a program that compiled, linked and
 * ran.
 */
static void print_recipe_toml(capability_lang lang, const link_recipe *recipe) {
    printf("\n[%s]\n", lang == lang_cxx ? "cxx" : "c");

    /* Which standard library the flags commit the build to. Published for C++
       because it is an ABI, not a preference: objects built against libc++ and
       against libstdc++ cannot be linked together, so a caller pulling in a
       prebuilt library has to know which one it is looking at. */
    if (lang == lang_cxx)
        printf("stdlib = \"%s\"\n", recipe_stdlib_name(recipe->stdlib));

    print_string_array("compile_flags", recipe->compile_flags, recipe->compile_count);
    print_string_array("link_flags", recipe->link_flags, recipe->link_count);
    /* Where the shared libraries the produced program needs actually live.
       Linking is not running, and a caller that has to launch what it built,
       or ship it, cannot derive these from the flags. */
    print_dir_array("runtime_dirs", recipe->runtime_dirs, recipe->runtime_count);
}

static void print_toml(const toolchain *chain, capability_lang lang,
                       const char *standard, const link_recipe *recipe) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);

    printf("[compiler]\n");
    /* The identity, so a caller can name this same toolchain again to `show`
       or to a later resolve without holding on to a path. */
    printf("id = \"%s\"\n", chain->id);
    printf("path = \"%s\"\n", lang == lang_cxx ? chain->cxx_path : chain->path);
    /* Both drivers, always. A project with C and C++ sources needs each one,
       and they must come from the same toolchain; `path` alone answers only
       the language that was asked about. */
    printf("c_path = \"%s\"\n", chain->path);
    printf("cxx_path = \"%s\"\n", chain->cxx_path);
    printf("vendor = \"%s\"\n", toolchain_vendor_name(chain->vendor));
    printf("version = \"%s\"\n", version);
    printf("target = \"%s\"\n", chain->target);
    if (standard != NULL)
        printf("std_flag = \"-std=%s\"\n", standard);

    print_recipe_toml(lang, recipe);
}

int resolve_command_run(const resolve_request *request, bool as_toml) {
    capability_lang lang;
    if (!parse_lang(request->lang, &lang)) {
        fprintf(stderr, "pickup: unknown language '%s'\n", request->lang);
        return exit_usage_error;
    }
    if (request->vendor != NULL
        && toolchain_vendor_parse(request->vendor) == vendor_unknown) {
        fprintf(stderr, "pickup: unknown vendor '%s'\n", request->vendor);
        return exit_usage_error;
    }

    cxx_stdlib wanted_stdlib;
    if (!parse_stdlib(request->stdlib, &wanted_stdlib)) {
        fprintf(stderr, "pickup: unknown standard library '%s'\n", request->stdlib);
        return exit_usage_error;
    }
    /* Naming a C++ standard library while asking for C is a request that
       cannot be honoured, and honouring it silently would leave the caller
       believing a constraint was applied. */
    if (wanted_stdlib != stdlib_unknown && lang != lang_cxx) {
        fprintf(stderr, "pickup: --stdlib applies to c++, not %s\n", LANG_C_NAME);
        return exit_usage_error;
    }

    capability_set required = { 0 };
    if (request->features != NULL) {
        char unknown[FEATURE_ID_SIZE] = "";
        if (!add_named_features(request->features, &required, unknown, sizeof unknown)) {
            fprintf(stderr, "pickup: unknown feature '%s'\n", unknown);
            return exit_usage_error;
        }
    }

    /* The request is checked before the machine is: rejecting an unknown
       feature after a minute of probing would be a minute spent on an answer
       that was never going to come. */
    inventory list;
    if (!probe_progress_load(&list, false)) {
        fprintf(stderr, "pickup: could not scan for compilers\n");
        return exit_failure;
    }

    link_recipe recipe;
    const toolchain *best = select_buildable(&list, request, lang, required,
                                             wanted_stdlib, &recipe);
    if (best == NULL) {
        fprintf(stderr, "pickup: no %s compiler satisfies the request\n",
                lang == lang_cxx ? LANG_CXX_NAME : LANG_C_NAME);
        for (size_t i = 0; i < list.count; i++)
            print_missing(&list.items[i], lang, required, request->standard);
        inventory_free(&list);
        return exit_no_match;
    }

    if (as_toml)
        print_toml(best, lang, request->standard, &recipe);
    else
        print_text(best, lang);
    inventory_free(&list);
    return exit_ok;
}
