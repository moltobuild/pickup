#include <pickup/services/inventory_service.h>

#include <pickup/detect/probe.h>
#include <pickup/detect/scanner.h>
#include <pickup/services/cache_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/util/str_list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many toolchains the inventory holds before it grows. */
#define INVENTORY_INITIAL_CAPACITY 8
#define INVENTORY_GROWTH_FACTOR 2

static bool inventory_grow(inventory *list) {
    size_t next =
        list->capacity == 0 ? INVENTORY_INITIAL_CAPACITY : list->capacity * INVENTORY_GROWTH_FACTOR;
    toolchain *items = realloc(list->items, next * sizeof(toolchain));
    if (items == NULL)
        return false;
    list->items = items;
    list->capacity = next;
    return true;
}

bool inventory_append(inventory *list, const toolchain *chain) {
    if (list->count == list->capacity && !inventory_grow(list))
        return false;
    list->items[list->count] = *chain;
    list->count++;
    return true;
}

/*
 * Which of two spellings of the same compiler to keep.
 *
 * A single GCC answers to `cc`, `gcc-9`, `c89-gcc` and `g++-9`. They are one
 * toolchain, and the one worth recording is the plain C driver: the C++ driver
 * is found by transforming its name, so `gcc` leads to `g++` while `c++` leads
 * nowhere — it is already the C++ side. Keep the wrong one and the toolchain
 * ends up recorded as having no C++ at all.
 */
static const char *const preferred_names[] = {"gcc", "clang", "cc"};
#define PREFERRED_COUNT (sizeof preferred_names / sizeof preferred_names[0])

/* Names that are already the C++ side, and so lead nowhere when the C++ driver
   is looked for by transforming them. */
static bool names_a_cxx_driver(const char *name) {
    return strcmp(name, "c++") == 0 || strncmp(name, "g++", 3) == 0 ||
           strncmp(name, "clang++", 7) == 0;
}

/* Lower is better. Decided from the name alone, because this runs before
   anything has been probed. */
static int name_rank(const toolchain *chain) {
    for (size_t i = 0; i < PREFERRED_COUNT; i++) {
        if (strcmp(chain->name, preferred_names[i]) == 0)
            return (int)i;
    }
    /* Anything is better than a C++ driver: `gcc` leads to `g++`, and `g++`
       leads nowhere. */
    return names_a_cxx_driver(chain->name) ? (int)PREFERRED_COUNT + 1 : (int)PREFERRED_COUNT;
}

/* True if both spellings are the same installed compiler.

   Identity is what it is and where it came from, not where a link to it sits:
   two drivers reporting the same vendor, version and target are one toolchain,
   while the system's gcc 12.3.0 and a conda one of the same version are not,
   because their targets differ. */
static bool same_toolchain(const toolchain *a, const toolchain *b) {
    return a->vendor == b->vendor && toolchain_version_compare(a->version, b->version) == 0 &&
           strcmp(a->target, b->target) == 0 && a->source == b->source;
}

/*
 * Fold `other` into `into`, keeping the best of both.
 *
 * Features are combined rather than replaced. Each spelling was probed on its
 * own, and a `c++` has no C++ driver to derive so its C++ features came back
 * empty, while the `cc` beside it found one and filled them in. Taking either
 * alone would report less than was actually proven.
 */
static void merge_into(toolchain *into, const toolchain *other) {
    /* Everything proven about either spelling, kept. */
    capability_set c = {into->c_features.bits | other->c_features.bits};
    capability_set cxx = {into->cxx_features.bits | other->cxx_features.bits};

    const toolchain *better = name_rank(other) < name_rank(into) ? other : into;
    const toolchain *worse = better == other ? into : other;

    char cxx_path[PICKUP_PATH_MAX];
    snprintf(cxx_path, sizeof cxx_path, "%s",
             better->cxx_path[0] != '\0' ? better->cxx_path : worse->cxx_path);

    if (better != into)
        *into = *better;
    into->c_features = c;
    into->cxx_features = cxx;
    snprintf(into->cxx_path, sizeof into->cxx_path, "%s", cxx_path);
}

/* Collapse the aliases of each compiler into one entry. */
static void inventory_merge(inventory *list) {
    size_t kept = 0;
    for (size_t i = 0; i < list->count; i++) {
        size_t existing = kept;
        for (size_t j = 0; j < kept; j++) {
            if (same_toolchain(&list->items[j], &list->items[i])) {
                existing = j;
                break;
            }
        }
        if (existing < kept)
            merge_into(&list->items[existing], &list->items[i]);
        else
            list->items[kept++] = list->items[i];
    }
    list->count = kept;

    /* Named once the merging is done, so the identity describes what was
       kept rather than whichever alias happened to be seen first. */
    for (size_t i = 0; i < list->count; i++)
        toolchain_make_id(&list->items[i]);
}

/* Order: vendor name, then newest version first, then path. Sorting by
   something stable is what makes two runs on one machine agree. */
static int compare_toolchains(const void *left, const void *right) {
    const toolchain *a = left;
    const toolchain *b = right;

    int by_vendor = strcmp(toolchain_vendor_name(a->vendor), toolchain_vendor_name(b->vendor));
    if (by_vendor != 0)
        return by_vendor;

    int by_version = toolchain_version_compare(b->version, a->version); /* descending */
    if (by_version != 0)
        return by_version;

    return strcmp(a->path, b->path);
}

/* Defined below, beside the rule that decides where a toolchain came from. */
static void assign_sources(inventory *list);

static void notify(inventory_watch watch, void *context, size_t done, size_t total) {
    if (watch != NULL)
        watch(done, total, context);
}

/* Probe every candidate and fill `out`. Shared by the cached and uncached
   paths, which differ only in where the candidate list comes from. */
static bool probe_candidates(const str_list *candidates, inventory *out, inventory_watch watch,
                             void *context) {
    /*
     * Identify everything first, collapse the aliases, and only then ask what
     * is left to compile.
     *
     * The two steps cost wildly different amounts. Asking a compiler what it
     * is takes two invocations; asking what it can compile takes one per
     * feature in the catalog. A machine offering `cc`, `gcc-9`, `c89-gcc` and
     * `g++-9` has one GCC behind them, and probing each of the four means
     * doing the expensive half four times to arrive at a single answer that
     * would then be merged anyway.
     */
    size_t total = str_list_count(candidates);
    bool ok = true;
    for (size_t i = 0; ok && i < total; i++) {
        toolchain chain;
        /* A candidate that cannot describe itself is not a compiler we can
           use; skipping it is the answer, not an error. */
        if (!probe_identify(str_list_get(candidates, i), &chain))
            continue;
        ok = inventory_append(out, &chain);
    }
    if (!ok) {
        inventory_free(out);
        return false;
    }

    assign_sources(out);
    inventory_merge(out);

    /* Now the slow half, once per compiler rather than once per name it
       answers to. The count reported is the number of compilers, which is also
       what is actually being worked through. */
    for (size_t i = 0; i < out->count; i++) {
        /* Announced before the work rather than after it, so the first
           compiler is not probed in silence. */
        notify(watch, context, i, out->count);
        probe_find_cxx_driver(&out->items[i]);
        probe_capabilities(&out->items[i]);
    }
    notify(watch, context, out->count, out->count);
    return true;
}

/* Everything worth probing: what PATH offers, plus the toolchains Pickup
   installed, which are not on PATH and would otherwise be invisible to the
   commands that report them (spec.md section 6). */
static bool collect_candidates(str_list *out) {
    str_list_init(out);
    if (!scanner_collect(getenv("PATH"), out))
        return false;

    char toolchains[PICKUP_PATHS_MAX];
    if (!paths_toolchains(toolchains, sizeof toolchains))
        return true; /* no pickup home: nothing of ours to add */
    return scanner_collect_installed(toolchains, out);
}

/* Where a toolchain came from, decided by where it lives.

   Anything under the toolchains directory is one Pickup installed and can
   remove again; everything else was already on the machine. Worked out rather
   than stored, because it depends on PICKUP_HOME, which a caller may point
   somewhere else between two runs. */
static toolchain_source source_of(const char *path) {
    char toolchains[PICKUP_PATHS_MAX];
    if (!paths_toolchains(toolchains, sizeof toolchains))
        return toolchain_source_system;

    size_t length = strlen(toolchains);
    if (length == 0 || strncmp(path, toolchains, length) != 0)
        return toolchain_source_system;
    /* A prefix match alone would claim /home/x/.pickup/toolchains-old too. */
    return path[length] == '/' ? toolchain_source_pickup : toolchain_source_system;
}

/* Where each entry came from. Split out because the merging needs it: two
   compilers are the same toolchain only if they came from the same place. */
static void assign_sources(inventory *list) {
    for (size_t i = 0; i < list->count; i++)
        list->items[i].source = source_of(list->items[i].path);
}

/* Done in one place because it has to happen identically whether the entries
   were just probed or read back from the cache: an inventory deduplicated on
   one path and not the other would describe a different machine depending on
   whether anything had changed since the last run. */
void inventory_settle(inventory *list) {
    assign_sources(list);
    inventory_merge(list);
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(toolchain), compare_toolchains);
}

bool inventory_discover(inventory *out) {
    memset(out, 0, sizeof *out);

    str_list candidates;
    if (!collect_candidates(&candidates)) {
        str_list_free(&candidates);
        return false;
    }
    bool ok = probe_candidates(&candidates, out, NULL, NULL);
    str_list_free(&candidates);
    if (ok)
        inventory_settle(out);
    return ok;
}

bool inventory_load(inventory *out, bool refresh) {
    return inventory_load_watched(out, refresh, NULL, NULL);
}

bool inventory_load_watched(inventory *out, bool refresh, inventory_watch watch, void *context) {
    memset(out, 0, sizeof *out);

    /* Scanning PATH is cheap; probing is not. So the candidate list is always
       rebuilt, and it is what tells the cache whether it is still current. */
    str_list candidates;
    if (!collect_candidates(&candidates)) {
        str_list_free(&candidates);
        return false;
    }

    if (refresh)
        cache_discard();
    else if (cache_load(&candidates, out)) {
        str_list_free(&candidates);
        inventory_settle(out);
        return true;
    }

    bool ok = probe_candidates(&candidates, out, watch, context);
    if (ok) {
        inventory_settle(out);
        /* A cache that cannot be written costs a rescan next time and nothing
           else, so it does not fail the command. */
        (void)cache_store(&candidates, out);
    }
    str_list_free(&candidates);
    return ok;
}

void inventory_free(inventory *list) {
    free(list->items);
    memset(list, 0, sizeof *list);
}

const toolchain *inventory_find(const inventory *list, const char *name) {
    /* The inventory is ordered newest first within a vendor, so the first
       match is also the highest version. That is what makes `gcc` and
       `gcc@latest` mean what a person expects them to mean, and `gcc@12` the
       newest 12 rather than an arbitrary one. */
    for (size_t i = 0; i < list->count; i++) {
        if (toolchain_matches(&list->items[i], name))
            return &list->items[i];
    }
    return NULL;
}

size_t inventory_count_matching(const inventory *list, const char *name) {
    size_t count = 0;
    for (size_t i = 0; i < list->count; i++) {
        if (toolchain_matches(&list->items[i], name))
            count++;
    }
    return count;
}
