#include <pickup/services/inventory_service.h>

#include <pickup/detect/probe.h>
#include <pickup/detect/scanner.h>
#include <pickup/services/cache_service.h>
#include <pickup/util/str_list.h>

#include <stdlib.h>
#include <string.h>

/* How many toolchains the inventory holds before it grows. */
#define INVENTORY_INITIAL_CAPACITY 8
#define INVENTORY_GROWTH_FACTOR    2

static bool inventory_grow(inventory *list) {
    size_t next = list->capacity == 0
        ? INVENTORY_INITIAL_CAPACITY
        : list->capacity * INVENTORY_GROWTH_FACTOR;
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

/* Order: vendor name, then newest version first, then path. Sorting by
   something stable is what makes two runs on one machine agree. */
static int compare_toolchains(const void *left, const void *right) {
    const toolchain *a = left;
    const toolchain *b = right;

    int by_vendor = strcmp(toolchain_vendor_name(a->vendor),
                           toolchain_vendor_name(b->vendor));
    if (by_vendor != 0)
        return by_vendor;

    int by_version = toolchain_version_compare(b->version, a->version); /* descending */
    if (by_version != 0)
        return by_version;

    return strcmp(a->path, b->path);
}

/* Probe every candidate and fill `out`. Shared by the cached and uncached
   paths, which differ only in where the candidate list comes from. */
static bool probe_candidates(const str_list *candidates, inventory *out) {
    bool ok = true;
    for (size_t i = 0; ok && i < str_list_count(candidates); i++) {
        toolchain chain;
        /* A candidate that cannot describe itself is not a compiler we can
           use; skipping it is the answer, not an error. */
        if (!probe_identify(str_list_get(candidates, i), &chain))
            continue;
        probe_find_cxx_driver(&chain);
        probe_capabilities(&chain);
        ok = inventory_append(out, &chain);
    }
    if (!ok) {
        inventory_free(out);
        return false;
    }
    return true;
}

/* Put the inventory in its documented order, so two runs agree. */
static void inventory_sort(inventory *list) {
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(toolchain), compare_toolchains);
}

bool inventory_discover(inventory *out) {
    memset(out, 0, sizeof *out);

    str_list candidates;
    str_list_init(&candidates);
    if (!scanner_collect(getenv("PATH"), &candidates)) {
        str_list_free(&candidates);
        return false;
    }
    bool ok = probe_candidates(&candidates, out);
    str_list_free(&candidates);
    if (ok)
        inventory_sort(out);
    return ok;
}

bool inventory_load(inventory *out, bool refresh) {
    memset(out, 0, sizeof *out);

    /* Scanning PATH is cheap; probing is not. So the candidate list is always
       rebuilt, and it is what tells the cache whether it is still current. */
    str_list candidates;
    str_list_init(&candidates);
    if (!scanner_collect(getenv("PATH"), &candidates)) {
        str_list_free(&candidates);
        return false;
    }

    if (refresh)
        cache_discard();
    else if (cache_load(&candidates, out)) {
        str_list_free(&candidates);
        inventory_sort(out);
        return true;
    }

    bool ok = probe_candidates(&candidates, out);
    if (ok) {
        inventory_sort(out);
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
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].name, name) == 0
            || strcmp(list->items[i].path, name) == 0)
            return &list->items[i];
    }
    return NULL;
}
