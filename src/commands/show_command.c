#include <pickup/commands/show_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>

#include <stdio.h>
#include <string.h>

/* Print the features of one language, one per line, so a reader can see
   exactly what was proven rather than a summary that hides the gaps. */
static void print_features(const toolchain *chain, capability_lang lang) {
    size_t count = 0;
    const capability *catalog = capability_catalog(&count);
    capability_set proven = lang == lang_c ? chain->c_features : chain->cxx_features;

    const char *current_standard = NULL;
    for (size_t i = 0; i < count; i++) {
        if (catalog[i].lang != lang)
            continue;
        if (current_standard == NULL || strcmp(current_standard, catalog[i].standard) != 0) {
            current_standard = catalog[i].standard;
            printf("  %s\n", current_standard);
        }
        printf("    %-24s %s\n", catalog[i].id,
               capability_set_has(proven, i) ? "yes" : "no");
    }
}

static void print_text(const toolchain *chain) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);

    /* Headed by the identity rather than the basename of whichever link was
       found: it is what names this toolchain to every other command. */
    printf("%s\n", chain->id);
    printf("  path     %s\n", chain->path);
    printf("  vendor   %s\n", toolchain_vendor_name(chain->vendor));
    printf("  version  %s\n", version);
    printf("  target   %s\n", chain->target[0] != '\0' ? chain->target : "-");
    printf("  source   %s\n", toolchain_source_name(chain->source));
    printf("  c++      %s\n", chain->cxx_path[0] != '\0' ? chain->cxx_path : "(none found)");

    printf("\nC features\n");
    print_features(chain, lang_c);
    if (chain->cxx_path[0] != '\0') {
        printf("\nC++ features\n");
        print_features(chain, lang_cxx);
    }
}

static void print_toml(const toolchain *chain) {
    char version[32];
    toolchain_version_format(chain->version, version, sizeof version);

    printf("[toolchain]\n");
    printf("id = \"%s\"\n", chain->id);
    printf("name = \"%s\"\n", chain->name);
    printf("path = \"%s\"\n", chain->path);
    printf("vendor = \"%s\"\n", toolchain_vendor_name(chain->vendor));
    printf("version = \"%s\"\n", version);
    printf("target = \"%s\"\n", chain->target);
    printf("source = \"%s\"\n", toolchain_source_name(chain->source));
    printf("cxx_path = \"%s\"\n", chain->cxx_path);

    size_t count = 0;
    const capability *catalog = capability_catalog(&count);
    for (capability_lang lang = lang_c; lang <= lang_cxx; lang++) {
        capability_set proven = lang == lang_c ? chain->c_features : chain->cxx_features;
        printf("%s = [", lang == lang_c ? "c_features" : "cxx_features");
        bool first = true;
        for (size_t i = 0; i < count; i++) {
            if (catalog[i].lang != lang || !capability_set_has(proven, i))
                continue;
            printf("%s\"%s\"", first ? "" : ", ", catalog[i].id);
            first = false;
        }
        printf("]\n");
    }
}

/*
 * Say so when the query could have meant more than one thing.
 *
 * Asking for `gcc@12` on a machine with a 12.4 and a 12.3 is not ambiguous:
 * the newest is the obvious answer and the one given. Asking for it where two
 * toolchains are both 12.3.0 is, because there is nothing to rank them by, and
 * silently picking one would leave a reader looking at a compiler they did not
 * mean while believing they had named it.
 *
 * The chosen one is still shown. The warning goes to stderr, so a script
 * reading the answer is unaffected by it.
 */
static void warn_if_ambiguous(const inventory *list, const char *query,
                              const toolchain *chosen) {
    size_t tied = 0;
    for (size_t i = 0; i < list->count; i++) {
        if (toolchain_matches(&list->items[i], query)
            && toolchain_version_compare(list->items[i].version, chosen->version) == 0)
            tied++;
    }
    if (tied < 2)
        return;

    fprintf(stderr, "pickup: '%s' names %zu toolchains; showing %s\n",
            query, tied, chosen->id);
    for (size_t i = 0; i < list->count; i++) {
        if (toolchain_matches(&list->items[i], query)
            && toolchain_version_compare(list->items[i].version, chosen->version) == 0)
            fprintf(stderr, "  %s  (%s)\n", list->items[i].id,
                    toolchain_source_name(list->items[i].source));
    }
}

int show_command_run(const char *name, bool as_toml) {
    if (name == NULL) {
        fprintf(stderr, "pickup: show needs a toolchain name\n");
        return exit_usage_error;
    }

    inventory list;
    if (!inventory_load(&list, false)) {
        fprintf(stderr, "pickup: could not scan for compilers\n");
        return exit_failure;
    }

    const toolchain *chain = inventory_find(&list, name);
    if (chain == NULL) {
        fprintf(stderr, "pickup: no toolchain named '%s'\n", name);
        inventory_free(&list);
        return exit_no_match;
    }
    warn_if_ambiguous(&list, name, chain);

    if (as_toml)
        print_toml(chain);
    else
        print_text(chain);
    inventory_free(&list);
    return exit_ok;
}
