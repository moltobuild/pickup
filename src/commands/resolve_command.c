#include <pickup/commands/resolve_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>

#include <stdio.h>
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

static void print_text(const toolchain *chain, capability_lang lang) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);
    printf("%s %s %s (%s)\n", chain->name, toolchain_vendor_name(chain->vendor),
           version, lang == lang_cxx ? chain->cxx_path : chain->path);
}

static void print_toml(const toolchain *chain, capability_lang lang,
                       const char *standard) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);

    printf("[compiler]\n");
    printf("path = \"%s\"\n", lang == lang_cxx ? chain->cxx_path : chain->path);
    printf("cxx_path = \"%s\"\n", chain->cxx_path);
    printf("vendor = \"%s\"\n", toolchain_vendor_name(chain->vendor));
    printf("version = \"%s\"\n", version);
    printf("target = \"%s\"\n", chain->target);
    if (standard != NULL)
        printf("std_flag = \"-std=%s\"\n", standard);
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

    capability_set required = { 0 };
    if (request->features != NULL) {
        char unknown[FEATURE_ID_SIZE] = "";
        if (!add_named_features(request->features, &required, unknown, sizeof unknown)) {
            fprintf(stderr, "pickup: unknown feature '%s'\n", unknown);
            return exit_usage_error;
        }
    }

    inventory list;
    if (!inventory_load(&list, false)) {
        fprintf(stderr, "pickup: could not scan for compilers\n");
        return exit_failure;
    }

    /* Highest version wins. The inventory is already ordered, so scanning for
       the best match yields the same answer on every run. */
    const toolchain *best = NULL;
    for (size_t i = 0; i < list.count; i++) {
        const toolchain *chain = &list.items[i];
        if (!satisfies(chain, request, lang, required))
            continue;
        if (best == NULL
            || toolchain_version_compare(chain->version, best->version) > 0)
            best = chain;
    }

    if (best == NULL) {
        fprintf(stderr, "pickup: no %s compiler satisfies the request\n",
                lang == lang_cxx ? LANG_CXX_NAME : LANG_C_NAME);
        for (size_t i = 0; i < list.count; i++)
            print_missing(&list.items[i], lang, required, request->standard);
        inventory_free(&list);
        return exit_no_match;
    }

    if (as_toml)
        print_toml(best, lang, request->standard);
    else
        print_text(best, lang);
    inventory_free(&list);
    return exit_ok;
}
