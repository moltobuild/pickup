#include <pickup/sources/conda_closure.h>

#include <pickup/services/fs_service.h>

#include <string.h>

/*
 * Requirements waiting to be met, and who asked for each.
 *
 * Sized well above the number of packages a closure ends up with, because most
 * requirements are for something already chosen: eleven packages of LLVM name
 * zstd, libgcc and libstdcxx between them a dozen times over. A queue the size
 * of the answer overflows long before the walk is finished, and the overflow
 * looks exactly like a package that could not be found.
 */
#define CONDA_MAX_QUEUE 512

typedef struct {
    char specs[CONDA_MAX_QUEUE][CONDA_SPEC_MAX];
    char askers[CONDA_MAX_QUEUE][CONDA_NAME_MAX];
    size_t count;
    size_t taken;
} spec_queue;

/* How a package is looked up: from the channel, or from a list in hand. */
typedef struct {
    const conda_list *available;   /* NULL when the channel is to be asked */
    bool refresh;
    conda_watch watch;             /* may be NULL */
    void *context;
} resolver;

void conda_closure_init(conda_closure *closure) {
    conda_list_init(&closure->packages);
    closure->complete = true;
    closure->missing[0] = '\0';
    closure->required_by[0] = '\0';
}

void conda_closure_free(conda_closure *closure) {
    conda_list_free(&closure->packages);
}

long long conda_closure_size(const conda_closure *closure) {
    long long total = 0;
    for (size_t i = 0; i < closure->packages.count; i++)
        total += closure->packages.items[i].size;
    return total;
}

static bool queue_push(spec_queue *queue, const char *spec, const char *asker) {
    if (queue->count == CONDA_MAX_QUEUE)
        return false;
    (void)fs_format_path(queue->specs[queue->count], CONDA_SPEC_MAX, "%s", spec);
    (void)fs_format_path(queue->askers[queue->count], CONDA_NAME_MAX, "%s",
                         asker != NULL ? asker : "");
    queue->count++;
    return true;
}

/* True if a package of this name is already part of the closure. */
static const conda_package *already_chosen(const conda_closure *closure,
                                           const char *name) {
    for (size_t i = 0; i < closure->packages.count; i++) {
        if (strcmp(closure->packages.items[i].name, name) == 0)
            return &closure->packages.items[i];
    }
    return NULL;
}

/* The newest build of `spec` among what is available.

   The candidates arrive newest first, so the first that satisfies the
   constraint is the one to take. No search and no backtracking: the channel
   built these against each other, and a toolchain whose constraints need
   searching is one Pickup should refuse rather than guess at. */
static bool choose(const resolver *how, const conda_spec *spec, conda_package *out) {
    if (how->available != NULL) {
        for (size_t i = 0; i < how->available->count; i++) {
            if (conda_spec_matches(spec, &how->available->items[i])) {
                *out = how->available->items[i];
                return true;
            }
        }
        return false;
    }

    conda_list candidates;
    if (!conda_fetch_packages(spec->name, NULL, how->refresh, &candidates))
        return false;

    bool found = false;
    for (size_t i = 0; i < candidates.count && !found; i++) {
        if (conda_spec_matches(spec, &candidates.items[i])) {
            *out = candidates.items[i];
            found = true;
        }
    }
    conda_list_free(&candidates);
    return found;
}

/* Record that the walk could not go on, and why. */
static void give_up(conda_closure *closure, const char *spec, const char *asker) {
    closure->complete = false;
    (void)fs_format_path(closure->missing, sizeof closure->missing, "%s", spec);
    (void)fs_format_path(closure->required_by, sizeof closure->required_by, "%s",
                         asker != NULL ? asker : "");
}

/* Walk the requirements, adding a package for each and following what it
   names. */
static bool walk(const resolver *how, spec_queue *queue, conda_closure *out) {
    while (queue->taken < queue->count) {
        const char *text = queue->specs[queue->taken];
        const char *asker = queue->askers[queue->taken];
        queue->taken++;

        conda_spec spec;
        if (!conda_spec_parse(text, &spec)) {
            /* A form Pickup does not read is not treated as satisfied: that
               would install a toolchain missing whatever it named. */
            give_up(out, text, asker);
            return true;
        }

        /* A requirement about the machine rather than about a package: there
           is nothing to fetch, and treating it as missing would refuse an
           install over something already provided. */
        if (conda_is_virtual(spec.name))
            continue;

        const conda_package *chosen = already_chosen(out, spec.name);
        if (chosen != NULL) {
            /* Already in, from an earlier requirement. If it does not also
               satisfy this one, two packages disagree about what they need and
               there is nothing here that resolves that. */
            if (!conda_spec_matches(&spec, chosen))
                give_up(out, text, asker);
            continue;
        }

        /* Said before the lookup rather than after it, because the lookup is
           the part that takes the time. */
        if (how->watch != NULL)
            how->watch(spec.name, out->packages.count, how->context);

        conda_package package;
        if (!choose(how, &spec, &package)) {
            give_up(out, text, asker);
            return true;
        }
        if (!conda_list_push(&out->packages, &package))
            return false;

        for (size_t i = 0; i < package.depend_count; i++) {
            if (!queue_push(queue, package.depends[i], package.name)) {
                /* Not a missing package: more requirements than the walk was
                   built to hold. Saying "missing" here would send a reader
                   looking for something that is on the channel. */
                give_up(out, "too many requirements to follow", package.name);
                return true;
            }
        }
    }
    return true;
}

/* Start the walk from the root package, at whichever version was asked for. */
static bool begin(const resolver *how, const char *root, const char *version_filter,
                  conda_closure *out) {
    conda_closure_init(out);

    spec_queue queue = { .count = 0, .taken = 0 };
    char spec[CONDA_SPEC_MAX];
    /* A version filter becomes the root's own constraint, so the walk starts
       from the same kind of requirement as every step that follows. */
    bool composed = version_filter != NULL && version_filter[0] != '\0'
        ? fs_format_path(spec, sizeof spec, "%s %s.*", root, version_filter)
        : fs_format_path(spec, sizeof spec, "%s", root);
    if (!composed || !queue_push(&queue, spec, NULL))
        return false;

    return walk(how, &queue, out);
}

bool conda_resolve(const char *root, const char *version_filter, bool refresh,
                   conda_closure *out) {
    return conda_resolve_watched(root, version_filter, refresh, NULL, NULL, out);
}

bool conda_resolve_watched(const char *root, const char *version_filter, bool refresh,
                           conda_watch watch, void *context, conda_closure *out) {
    const resolver how = {
        .available = NULL, .refresh = refresh, .watch = watch, .context = context,
    };
    return begin(&how, root, version_filter, out);
}

bool conda_resolve_from(const conda_list *available, const char *root,
                        const char *version_filter, conda_closure *out) {
    const resolver how = { .available = available, .refresh = false };
    return begin(&how, root, version_filter, out);
}
