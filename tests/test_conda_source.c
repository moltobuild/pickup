#include <moltest.h>

#include <pickup/sources/conda_closure.h>
#include <pickup/sources/conda_source.h>

#include <stdio.h>
#include <string.h>

/*
 * A recorded answer from the channel, cut down to the shape that matters.
 *
 * The subdirectories are the interesting part. `libgcc-devel_linux-64` is
 * published as noarch despite what its name says — it is headers and archives
 * with nothing to execute — so a resolver reading only the host's own
 * subdirectory finds the compiler and none of what it compiles against. That
 * is a toolchain which unpacks and cannot build, so it has to be got right,
 * and it is not something the machine running the tests can demonstrate.
 */
static const char channel_json[] =
    "{"
    "  \"name\": \"gcc_impl_linux-64\","
    "  \"files\": ["
    "    {\"basename\": \"linux-64/gcc_impl_linux-64-16.1.0-h5fcb69b_1.conda\","
    "     \"version\": \"16.1.0\", \"size\": 85161422,"
    "     \"attrs\": {\"subdir\": \"linux-64\", \"build\": \"h5fcb69b_1\","
    "                 \"timestamp\": 1785386591839,"
    "                 \"depends\": [\"libgcc >=16.1.0\","
    "                               \"libgcc-devel_linux-64 16.1.0 h59071f9_101\","
    "                               \"__glibc >=2.17,<3.0.a0\"]}},"
    "    {\"basename\": \"linux-64/gcc_impl_linux-64-16.1.0-he1f67d1_0.conda\","
    "     \"version\": \"16.1.0\", \"size\": 82487370,"
    "     \"attrs\": {\"subdir\": \"linux-64\", \"build\": \"he1f67d1_0\","
    "                 \"timestamp\": 1785269869623, \"depends\": []}},"
    "    {\"basename\": \"linux-64/gcc_impl_linux-64-12.4.0-h26ba24d_2.conda\","
    "     \"version\": \"12.4.0\", \"size\": 60400000,"
    "     \"attrs\": {\"subdir\": \"linux-64\", \"build\": \"h26ba24d_2\","
    "                 \"timestamp\": 1700000000000, \"depends\": []}},"
    "    {\"basename\": \"osx-64/gcc_impl_osx-64-16.1.0-h0c54a16_1.conda\","
    "     \"version\": \"16.1.0\", \"size\": 1,"
    "     \"attrs\": {\"subdir\": \"osx-64\", \"build\": \"h0c54a16_1\","
    "                 \"timestamp\": 1785386591839, \"depends\": []}}"
    "  ]"
    "}";

static bool host_is_linux_x64(void) {
#if defined(__linux__) && defined(__x86_64__)
    return true;
#else
    return false;
#endif
}

MOLTEST(conda_keeps_only_what_this_host_can_install) {
    if (!host_is_linux_x64())
        SKIP("the recorded subdirs are written for linux-64");

    conda_list list;
    ASSERT_TRUE(conda_parse_packages(channel_json, NULL, &list));

    /* The macOS build is gone; the three for this host remain. */
    ASSERT_EQ(3, (int)list.count);
    for (size_t i = 0; i < list.count; i++)
        EXPECT_TRUE(strstr(list.items[i].file, "osx") == NULL);

    conda_list_free(&list);
}

MOLTEST(conda_takes_the_later_build_of_the_same_version) {
    if (!host_is_linux_x64())
        SKIP("the recorded subdirs are written for linux-64");

    conda_list list;
    ASSERT_TRUE(conda_parse_packages(channel_json, "16", &list));
    ASSERT_EQ(2, (int)list.count);

    conda_package best;
    ASSERT_TRUE(conda_best(&list, &best));
    /* Two builds of one version differ in what they were built against, and
       the later one is what the rest of the channel now expects. */
    EXPECT_STREQ("h5fcb69b_1", best.build);
    EXPECT_EQ(3, (int)best.depend_count);

    conda_list_free(&list);
}

MOLTEST(conda_filters_by_a_partial_version) {
    if (!host_is_linux_x64())
        SKIP("the recorded subdirs are written for linux-64");

    conda_list list;
    ASSERT_TRUE(conda_parse_packages(channel_json, "12", &list));
    EXPECT_EQ(1, (int)list.count);
    conda_list_free(&list);

    ASSERT_TRUE(conda_parse_packages(channel_json, "99", &list));
    EXPECT_EQ(0, (int)list.count);
    conda_package best;
    EXPECT_FALSE(conda_best(&list, &best));
    conda_list_free(&list);
}

MOLTEST(conda_refuses_an_answer_that_is_not_json) {
    conda_list list;
    EXPECT_FALSE(conda_parse_packages("<html>rate limited</html>", NULL, &list));
    EXPECT_EQ(0, (int)list.count);
    conda_list_free(&list);
}

MOLTEST(conda_reads_every_form_of_specification_it_meets) {
    conda_spec spec;

    /* A bare name: any build will do. */
    ASSERT_TRUE(conda_spec_parse("sysroot_linux-64", &spec));
    EXPECT_STREQ("sysroot_linux-64", spec.name);
    EXPECT_STREQ("", spec.version);

    ASSERT_TRUE(conda_spec_parse("libgcc >=16.1.0", &spec));
    EXPECT_STREQ("libgcc", spec.name);
    EXPECT_STREQ("16.1.0", spec.version);
    EXPECT_TRUE(spec.at_least);

    /* A trailing .* means the components named and anything below them. */
    ASSERT_TRUE(conda_spec_parse("gcc_impl_linux-64 15.3.0.*", &spec));
    EXPECT_STREQ("15.3.0", spec.version);
    EXPECT_FALSE(spec.at_least);

    /* Version and build, which is how the channel pins two packages that were
       built against each other. */
    ASSERT_TRUE(conda_spec_parse("libsanitizer 16.1.0 hf2715c6_1", &spec));
    EXPECT_STREQ("16.1.0", spec.version);
    EXPECT_STREQ("hf2715c6_1", spec.build);

    /* A half-open range. The upper bound is kept as the channel wrote it,
       trailing prerelease marker and all, because what matters is how it
       compares rather than how it reads. */
    ASSERT_TRUE(conda_spec_parse("__glibc >=2.17,<3.0.a0", &spec));
    EXPECT_STREQ("2.17", spec.version);
    EXPECT_TRUE(strncmp(spec.version_below, "3.0", 3) == 0);
    EXPECT_TRUE(spec.at_least);
}

MOLTEST(conda_refuses_a_specification_it_cannot_read) {
    conda_spec spec;
    /* Approximating a constraint installs the wrong thing, so a form Pickup
       does not understand is refused rather than widened. */
    EXPECT_FALSE(conda_spec_parse("python !=3.9", &spec));
    EXPECT_FALSE(conda_spec_parse("thing >=1.0|<2.0", &spec));
    EXPECT_FALSE(conda_spec_parse("", &spec));
    EXPECT_FALSE(conda_spec_parse(NULL, &spec));
}

/* Build a package by hand, for matching tests. */
static conda_package package_of(const char *name, const char *version,
                                const char *build) {
    conda_package package = { 0 };
    snprintf(package.name, sizeof package.name, "%s", name);
    snprintf(package.version, sizeof package.version, "%s", version);
    snprintf(package.build, sizeof package.build, "%s", build);
    return package;
}

MOLTEST(conda_matches_a_specification_against_a_package) {
    conda_package libgcc = package_of("libgcc", "16.1.0", "ha9f2e26_1");
    conda_spec spec;

    ASSERT_TRUE(conda_spec_parse("libgcc >=16.1.0", &spec));
    EXPECT_TRUE(conda_spec_matches(&spec, &libgcc));

    ASSERT_TRUE(conda_spec_parse("libgcc >=17.0.0", &spec));
    EXPECT_FALSE(conda_spec_matches(&spec, &libgcc));

    /* An upper bound is exclusive. */
    ASSERT_TRUE(conda_spec_parse("libgcc >=16.0.0,<16.1.0", &spec));
    EXPECT_FALSE(conda_spec_matches(&spec, &libgcc));
    ASSERT_TRUE(conda_spec_parse("libgcc >=16.0.0,<17.0.0", &spec));
    EXPECT_TRUE(conda_spec_matches(&spec, &libgcc));

    /* A named build is exact. */
    ASSERT_TRUE(conda_spec_parse("libgcc 16.1.0 ha9f2e26_1", &spec));
    EXPECT_TRUE(conda_spec_matches(&spec, &libgcc));
    ASSERT_TRUE(conda_spec_parse("libgcc 16.1.0 something_else", &spec));
    EXPECT_FALSE(conda_spec_matches(&spec, &libgcc));

    /* A different package never matches, whatever the version says. */
    ASSERT_TRUE(conda_spec_parse("libstdcxx >=16.1.0", &spec));
    EXPECT_FALSE(conda_spec_matches(&spec, &libgcc));
}

MOLTEST(conda_knows_a_virtual_package_when_it_sees_one) {
    /* These describe the machine rather than anything to download, and
       treating one as missing would refuse an install over a requirement that
       was already met. */
    EXPECT_TRUE(conda_is_virtual("__glibc"));
    EXPECT_TRUE(conda_is_virtual("__unix"));
    EXPECT_FALSE(conda_is_virtual("libgcc"));
    EXPECT_FALSE(conda_is_virtual("_openmp_mutex"));
    EXPECT_FALSE(conda_is_virtual(NULL));
}

/* --- the closure --- */

/* Add a package with its dependencies to a list. */
static bool add(conda_list *list, const char *name, const char *version,
                const char *build, const char *first, const char *second) {
    conda_package package = package_of(name, version, build);
    package.size = 1000;
    if (first != NULL)
        snprintf(package.depends[package.depend_count++], CONDA_SPEC_MAX, "%s", first);
    if (second != NULL)
        snprintf(package.depends[package.depend_count++], CONDA_SPEC_MAX, "%s", second);
    return conda_list_push(list, &package);
}

MOLTEST(conda_gathers_everything_a_toolchain_needs) {
    conda_list available;
    conda_list_init(&available);
    ASSERT_TRUE(add(&available, "gxx", "16.1.0", "h1", "gcc 16.1.0", "libstdcxx >=16.1.0"));
    ASSERT_TRUE(add(&available, "gcc", "16.1.0", "h2", "libgcc >=16.1.0", NULL));
    ASSERT_TRUE(add(&available, "libgcc", "16.1.0", "h3", "__glibc >=2.17", NULL));
    ASSERT_TRUE(add(&available, "libstdcxx", "16.1.0", "h4", NULL, NULL));

    conda_closure closure;
    ASSERT_TRUE(conda_resolve_from(&available, "gxx", "16", &closure));

    /* The root, what it names, and what those name in turn. The virtual
       package is not one of them. */
    EXPECT_TRUE(closure.complete);
    EXPECT_EQ(4, (int)closure.packages.count);
    EXPECT_STREQ("gxx", closure.packages.items[0].name);
    EXPECT_TRUE(conda_closure_size(&closure) == 4000);

    conda_closure_free(&closure);
    conda_list_free(&available);
}

MOLTEST(conda_says_which_requirement_it_could_not_meet) {
    conda_list available;
    conda_list_init(&available);
    ASSERT_TRUE(add(&available, "gxx", "16.1.0", "h1", "libstdcxx >=16.1.0", NULL));
    /* Present, but older than what was asked for. */
    ASSERT_TRUE(add(&available, "libstdcxx", "15.0.0", "h4", NULL, NULL));

    conda_closure closure;
    ASSERT_TRUE(conda_resolve_from(&available, "gxx", NULL, &closure));

    /* Naming what is missing is part of the answer; installing most of a
       toolchain would be worse than installing none of it. */
    EXPECT_FALSE(closure.complete);
    EXPECT_TRUE(strstr(closure.missing, "libstdcxx") != NULL);
    EXPECT_STREQ("gxx", closure.required_by);

    conda_closure_free(&closure);
    conda_list_free(&available);
}

MOLTEST(conda_takes_each_package_once) {
    conda_list available;
    conda_list_init(&available);
    /* Both halves want libgcc, which must not be installed twice. */
    ASSERT_TRUE(add(&available, "gxx", "16.1.0", "h1", "gcc", "libgcc >=16.1.0"));
    ASSERT_TRUE(add(&available, "gcc", "16.1.0", "h2", "libgcc >=16.1.0", NULL));
    ASSERT_TRUE(add(&available, "libgcc", "16.1.0", "h3", NULL, NULL));

    conda_closure closure;
    ASSERT_TRUE(conda_resolve_from(&available, "gxx", NULL, &closure));
    EXPECT_TRUE(closure.complete);
    EXPECT_EQ(3, (int)closure.packages.count);

    conda_closure_free(&closure);
    conda_list_free(&available);
}

MOLTEST(conda_stops_when_two_packages_disagree) {
    conda_list available;
    conda_list_init(&available);
    /* One wants the new libgcc, the other pins the old one. There is nothing
       here that resolves that, and pretending otherwise would install a
       toolchain whose parts do not match. */
    ASSERT_TRUE(add(&available, "gxx", "16.1.0", "h1", "libgcc >=16.1.0",
                    "gcc 12.4.0"));
    ASSERT_TRUE(add(&available, "libgcc", "16.1.0", "h3", NULL, NULL));
    ASSERT_TRUE(add(&available, "gcc", "12.4.0", "h2", "libgcc 12.4.0", NULL));
    ASSERT_TRUE(add(&available, "libgcc", "12.4.0", "h5", NULL, NULL));

    conda_closure closure;
    ASSERT_TRUE(conda_resolve_from(&available, "gxx", NULL, &closure));
    EXPECT_FALSE(closure.complete);
    EXPECT_TRUE(strstr(closure.missing, "libgcc") != NULL);

    conda_closure_free(&closure);
    conda_list_free(&available);
}

MOLTEST(conda_resolves_nothing_from_an_empty_channel) {
    conda_list available;
    conda_list_init(&available);

    conda_closure closure;
    ASSERT_TRUE(conda_resolve_from(&available, "gxx", NULL, &closure));
    EXPECT_FALSE(closure.complete);
    EXPECT_EQ(0, (int)closure.packages.count);

    conda_closure_free(&closure);
    conda_list_free(&available);
}

MOLTEST(conda_names_a_subdirectory_for_this_host) {
    const char *subdir = conda_subdir();
    ASSERT_TRUE(subdir != NULL);
    EXPECT_TRUE(subdir[0] != '\0');
    if (host_is_linux_x64())
        EXPECT_STREQ("linux-64", subdir);
}
