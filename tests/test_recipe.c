#include <moltest.h>

#include <pickup/detect/recipe.h>
#include <pickup/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * What is under test is a preference, and a preference can only be observed
 * where more than one thing works. Real compilers on a real machine offer
 * whichever configurations that machine happens to allow, and usually only
 * one, so the drivers here are stand-ins: shell scripts that accept exactly
 * the flags they are told to accept and describe themselves the way a driver
 * does.
 *
 * They have to answer four different questions, because recipe_discover asks
 * four: which GCC installations exist (-v, on stderr), where its own libc++ is
 * (-print-file-name), which standard library a program ends up compiling
 * against, and whether that program links and runs.
 */

typedef struct {
    char root[64];
    /* What the toolchain owns. The same as the root for a fake standing in for
       a compiler the host installed, and a directory under a pickup home for
       one standing in for a toolchain pickup installed. */
    char prefix[PICKUP_PATHS_MAX];
    char driver[PICKUP_PATHS_MAX];
    char gcc_dir[PICKUP_PATHS_MAX];     /* the GCC installation it reports */
    char libcxx_dir[PICKUP_PATHS_MAX];  /* where its own libc++ sits */
    char libstdcxx_dir[PICKUP_PATHS_MAX];
} fake_toolchain;

/* What a fake is willing to accept. */
typedef struct {
    bool bare;             /* works with no flags at all */
    bool gcc_install;      /* works when given --gcc-install-dir= */
    bool libcxx;           /* works when given -stdlib=libc++ */
    bool ships_libcxx;     /* carries a libc++ to be pointed at */
    bool ships_libstdcxx;  /* carries a libstdc++ of its own */
} fake_accepts;

/* The pickup home a fake is judged against. Restored afterwards, because
   ownership is read from it and a leaked value would decide another test. */
typedef struct {
    char previous[PICKUP_PATHS_MAX];
    bool had_previous;
} home_fixture;

static bool home_setup(home_fixture *fixture, const char *home) {
    const char *existing = getenv(PICKUP_HOME_ENV);
    fixture->had_previous = existing != NULL;
    if (existing != NULL)
        snprintf(fixture->previous, sizeof fixture->previous, "%s", existing);
    return setenv(PICKUP_HOME_ENV, home, 1) == 0;
}

static void home_teardown(home_fixture *fixture) {
    if (fixture->had_previous)
        (void)setenv(PICKUP_HOME_ENV, fixture->previous, 1);
    else
        (void)unsetenv(PICKUP_HOME_ENV);
}

/* Lay out a prefix that looks enough like a GCC installation for the on-disk
   libstdc++ check to find headers in it. */
static bool make_gcc_tree(fake_toolchain *fake) {
    char headers[PICKUP_PATHS_MAX];
    if (!fs_format_path(fake->gcc_dir, sizeof fake->gcc_dir,
                        "%s/gccroot/lib/gcc/x86_64-linux-gnu/11", fake->root))
        return false;
    if (!fs_make_dirs(fake->gcc_dir))
        return false;
    if (!fs_format_path(headers, sizeof headers, "%s/gccroot/include/c++/11",
                        fake->root))
        return false;
    if (!fs_make_dirs(headers))
        return false;

    char iostream[PICKUP_PATHS_MAX];
    if (!fs_format_path(iostream, sizeof iostream, "%s/iostream", headers))
        return false;
    return fs_write_file(iostream, "/* a stand-in */\n");
}

/* Both of the toolchain's own libraries live inside its prefix, which is what
   makes them its own rather than the host's. */
static bool make_library(const char *directory, const char *name) {
    if (!fs_make_dirs(directory))
        return false;

    char library[PICKUP_PATHS_MAX];
    if (!fs_format_path(library, sizeof library, "%s/%s", directory, name))
        return false;
    return fs_write_file(library, "/* a stand-in */\n");
}

static bool make_libcxx(fake_toolchain *fake) {
    return fs_format_path(fake->libcxx_dir, sizeof fake->libcxx_dir,
                          "%s/lib", fake->prefix)
        && make_library(fake->libcxx_dir, "libc++.so");
}

static bool make_libstdcxx(fake_toolchain *fake) {
    return fs_format_path(fake->libstdcxx_dir, sizeof fake->libstdcxx_dir,
                          "%s/lib64", fake->prefix)
        && make_library(fake->libstdcxx_dir, "libstdc++.so");
}

/*
 * A driver that behaves as `accepts` says.
 *
 * Written in C rather than as a shell script: a fake has to be a program the
 * platform can start, and `#!/bin/sh` is not one on Windows. What it reads —
 * its arguments, its configuration file, the source on stdin — is what a real
 * driver reads, and the five things that vary between fixtures travel in the
 * spec beside it.
 */
static bool argument_given(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return true;
    }
    return false;
}

static bool argument_starts(int argc, char **argv, const char *prefix, const char **value) {
    const size_t width = strlen(prefix);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], prefix, width) == 0) {
            if (value != NULL)
                *value = argv[i] + width;
            return true;
        }
    }
    return false;
}

static const char *argument_after(int argc, char **argv, const char *flag) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    }
    return NULL;
}

static bool setting_is_yes(const char *key) {
    const char *value = moltest_fake_setting(key);
    return value != NULL && strcmp(value, "yes") == 0;
}

/* Reads the configuration file beside it, the way clang does, unless told not
   to. This is what makes a configured toolchain look like one needing no
   flags. */
static bool config_mentions(const char *driver, const char *text) {
    char path[PICKUP_PATHS_MAX];
    if (!fs_format_path(path, sizeof path, "%s.cfg", driver))
        return false;
    char *body = fs_read_file(path);
    if (body == NULL)
        return false;
    const bool found = strstr(body, text) != NULL;
    free(body);
    return found;
}

MOLTEST_FAKE(fake_driver) {
    const char *printname = NULL;
    bool gcc_install = argument_starts(argc, argv, "--gcc-install-dir=", NULL);
    bool libcxx = argument_given(argc, argv, "-stdlib=libc++");

    if (!argument_given(argc, argv, "--no-default-config")) {
        if (config_mentions(argv[0], "--gcc-install-dir="))
            gcc_install = true;
        if (config_mentions(argv[0], "-stdlib=libc++"))
            libcxx = true;
    }

    /* A driver answers with a full path for a library it has, and echoes the
       name back for one it does not. */
    if (argument_starts(argc, argv, "-print-file-name=", &printname)) {
        const char *dir = NULL;
        if (strcmp(printname, "libc++.so") == 0 && setting_is_yes("ships_libcxx"))
            dir = moltest_fake_setting("libcxx_dir");
        else if (strcmp(printname, "libstdc++.so") == 0 && setting_is_yes("ships_libstdcxx"))
            dir = moltest_fake_setting("libstdcxx_dir");
        if (dir != NULL)
            printf("%s/%s\n", dir, printname);
        else
            printf("%s\n", printname);
        return 0;
    }

    if (argument_given(argc, argv, "-v")) {
        const char *gcc_dir = moltest_fake_setting("gcc_dir");
        fprintf(stderr, "Found candidate GCC installation: %s\n", gcc_dir != NULL ? gcc_dir : "");
        fprintf(stderr, "Selected GCC installation: %s\n", gcc_dir != NULL ? gcc_dir : "");
        return 0;
    }

    /* Whether this invocation is one the driver is willing to serve. */
    const bool ok = (gcc_install && setting_is_yes("gcc_install")) ||
                    (libcxx && setting_is_yes("libcxx")) ||
                    (!gcc_install && !libcxx && setting_is_yes("bare"));

    /* A program that includes nothing needs no standard library, and a real
       compiler builds it whatever its libstdc++ situation. Only what reaches
       for a header depends on the flags. */
    const char *source = moltest_fake_input();
    if (strstr(source, "include") != NULL && !ok)
        return 1;

    /* A program asking which standard library this is gets answered the way a
       real one would: by failing to compile against the other. */
    if (strstr(source, "_LIBCPP_VERSION") != NULL && !libcxx)
        return 1;
    if (strstr(source, "__GLIBCXX__") != NULL && libcxx)
        return 1;

    if (argument_given(argc, argv, "-fsyntax-only"))
        return 0;

    const char *out = argument_after(argc, argv, "-o");
    if (out != NULL && !moltest_fake_program(out, "exit 0\n", NULL, 0))
        return 1;
    return 0;
}

static bool write_driver(fake_toolchain *fake, const fake_accepts *accepts) {
    char spec[1024];
    const int written = snprintf(spec, sizeof spec,
                                 "set ships_libcxx %s\n"
                                 "set ships_libstdcxx %s\n"
                                 "set gcc_install %s\n"
                                 "set libcxx %s\n"
                                 "set bare %s\n"
                                 "set libcxx_dir %s\n"
                                 "set libstdcxx_dir %s\n"
                                 "set gcc_dir %s\n"
                                 "behave fake_driver\n",
                                 accepts->ships_libcxx ? "yes" : "no",
                                 accepts->ships_libstdcxx ? "yes" : "no",
                                 accepts->gcc_install ? "yes" : "no",
                                 accepts->libcxx ? "yes" : "no", accepts->bare ? "yes" : "no",
                                 fake->libcxx_dir, fake->libstdcxx_dir, fake->gcc_dir);
    if (written < 0 || (size_t)written >= sizeof spec)
        return false;

    /* Where it actually landed is written back: the platform may have added a
       suffix, and every test below uses this path to run it. */
    char at[PICKUP_PATHS_MAX];
    snprintf(at, sizeof at, "%s", fake->driver);
    return moltest_fake_program(at, spec, fake->driver, sizeof fake->driver);
}

/* Everything both kinds of fake need, once the prefix and the driver are
   decided. The GCC tree stays under the root rather than the prefix: it stands
   in for the host's GCC, which is never the toolchain's own. */
static bool fake_build(fake_toolchain *fake, const fake_accepts *accepts) {
    return make_gcc_tree(fake) && make_libcxx(fake) && make_libstdcxx(fake)
        && write_driver(fake, accepts);
}

/* A compiler the host owns: nothing about it is under a pickup home. */
static bool fake_setup(fake_toolchain *fake, const fake_accepts *accepts) {
    if (!moltest_temp_dir("pickup_recipe", fake->root, sizeof fake->root))
        return false;
    if (!fs_format_path(fake->prefix, sizeof fake->prefix, "%s", fake->root))
        return false;
    if (!fs_format_path(fake->driver, sizeof fake->driver, "%s/fakecc", fake->root))
        return false;
    return fake_build(fake, accepts);
}

/* A toolchain pickup installed: a prefix under <PICKUP_HOME>/toolchains, with
   the driver in its bin the way a real one has. */
static bool fake_setup_installed(fake_toolchain *fake, const fake_accepts *accepts,
                                 home_fixture *home) {
    if (!moltest_temp_dir("pickup_recipe", fake->root, sizeof fake->root))
        return false;

    char home_dir[PICKUP_PATHS_MAX];
    char bin[PICKUP_PATHS_MAX];
    if (!fs_format_path(home_dir, sizeof home_dir, "%s/home", fake->root)
        || !home_setup(home, home_dir)
        || !fs_format_path(fake->prefix, sizeof fake->prefix,
                           "%s/toolchains/fake-1.0.0-x86_64-linux-gnu", home_dir)
        || !fs_format_path(bin, sizeof bin, "%s/bin", fake->prefix)
        || !fs_make_dirs(bin)
        || !fs_format_path(fake->driver, sizeof fake->driver, "%s/fakecc", bin))
        return false;
    return fake_build(fake, accepts);
}

static void fake_teardown(fake_toolchain *fake) {
    (void)fs_remove_tree(fake->root);
}

/* A toolchain whose C++ driver is the fake. */
static toolchain chain_of(const fake_toolchain *fake) {
    toolchain chain = { 0 };
    snprintf(chain.path, sizeof chain.path, "%s", fake->driver);
    snprintf(chain.cxx_path, sizeof chain.cxx_path, "%s", fake->driver);
    snprintf(chain.name, sizeof chain.name, "%s", "fakecc");
    chain.vendor = vendor_clang;
    return chain;
}

/* True if any published flag contains `fragment`. */
static bool has_compile_flag(const link_recipe *recipe, const char *fragment) {
    for (size_t i = 0; i < recipe->compile_count; i++) {
        if (strstr(recipe->compile_flags[i], fragment) != NULL)
            return true;
    }
    return false;
}

static bool has_link_flag(const link_recipe *recipe, const char *fragment) {
    for (size_t i = 0; i < recipe->link_count; i++) {
        if (strstr(recipe->link_flags[i], fragment) != NULL)
            return true;
    }
    return false;
}

MOLTEST(recipe_publishes_nothing_when_the_compiler_needs_nothing) {
    fake_toolchain fake;
    fake_accepts accepts = { .bare = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    /* No flags is the most portable answer there is, and a toolchain that
       works untouched must not be handed flags it never needed. */
    EXPECT_TRUE(recipe.usable);
    EXPECT_EQ(0, (int)recipe.compile_count);
    EXPECT_EQ(0, (int)recipe.link_count);
    EXPECT_EQ(0, (int)recipe.runtime_count);
    /* The library is still reported: it was read back, not assumed. */
    EXPECT_EQ(stdlib_libstdcxx, recipe.stdlib);

    fake_teardown(&fake);
}

MOLTEST(recipe_pins_the_gcc_installation_when_that_is_what_is_needed) {
    fake_toolchain fake;
    fake_accepts accepts = { .gcc_install = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    ASSERT_TRUE(recipe.usable);
    EXPECT_TRUE(has_compile_flag(&recipe, "--gcc-install-dir="));
    EXPECT_TRUE(has_compile_flag(&recipe, fake.gcc_dir));
    /* It belongs on both command lines: the headers come from there and so do
       the startup objects. */
    EXPECT_TRUE(has_link_flag(&recipe, "--gcc-install-dir="));
    EXPECT_EQ(stdlib_libstdcxx, recipe.stdlib);

    fake_teardown(&fake);
}

MOLTEST(recipe_falls_back_to_the_compilers_own_library) {
    fake_toolchain fake;
    fake_accepts accepts = { .libcxx = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(stdlib_libcxx, recipe.stdlib);
    EXPECT_TRUE(has_compile_flag(&recipe, "-stdlib=libc++"));

    /* The run-time search path is not decoration. Without it the link succeeds
       and the program dies looking for libc++.so, which is the failure this
       whole module exists to stop reporting as success. */
    EXPECT_TRUE(has_link_flag(&recipe, "-Wl,-rpath,"));
    EXPECT_FALSE(has_compile_flag(&recipe, "-Wl,-rpath,"));
    ASSERT_EQ(1, (int)recipe.runtime_count);
    EXPECT_STREQ(fake.libcxx_dir, recipe.runtime_dirs[0]);

    fake_teardown(&fake);
}

MOLTEST(recipe_prefers_the_host_library_for_a_compiler_the_host_owns) {
    fake_toolchain fake;
    /* The choice this module exists to make. Both configurations produce a
       running program; only one of them keeps the toolchain ABI-compatible
       with everything else already built on the machine. */
    fake_accepts accepts = { .gcc_install = true, .libcxx = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(stdlib_libstdcxx, recipe.stdlib);
    EXPECT_TRUE(has_compile_flag(&recipe, "--gcc-install-dir="));
    EXPECT_FALSE(has_compile_flag(&recipe, "-stdlib=libc++"));
    /* Nothing to carry: the system's library is already where the loader
       looks. */
    EXPECT_EQ(0, (int)recipe.runtime_count);

    fake_teardown(&fake);
}

MOLTEST(recipe_prefers_what_an_installed_toolchain_brought_with_it) {
    fake_toolchain fake;
    home_fixture home;
    /* The same compiler, the same two working configurations, and the opposite
       answer — because this one was unpacked into a prefix of its own. The
       libstdc++ it would otherwise borrow is whichever one this host happens to
       ship, and that would make one install a different compiler on every
       machine. */
    fake_accepts accepts = { .gcc_install = true, .libcxx = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup_installed(&fake, &accepts, &home));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(stdlib_libcxx, recipe.stdlib);
    EXPECT_TRUE(has_compile_flag(&recipe, "-stdlib=libc++"));
    ASSERT_EQ(1, (int)recipe.runtime_count);
    EXPECT_STREQ(fake.libcxx_dir, recipe.runtime_dirs[0]);

    /* The pinned GCC comes with it. libc++ still takes the startup objects and
       libgcc from one, and this is where a reordering that moved code instead
       of composing it first would quietly drop the flag. */
    EXPECT_TRUE(recipe_gcc_flag(&recipe) != NULL);

    fake_teardown(&fake);
    home_teardown(&home);
}

MOLTEST(recipe_prefers_its_own_library_to_needing_no_flags) {
    fake_toolchain fake;
    home_fixture home;
    /* Working untouched is not the same as working the same way everywhere. A
       host whose libstdc++ is new enough would have this compiler publish an
       empty recipe, and the next host would get something else. */
    fake_accepts accepts = { .bare = true, .libcxx = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup_installed(&fake, &accepts, &home));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(stdlib_libcxx, recipe.stdlib);
    EXPECT_EQ(1, (int)recipe.runtime_count);

    fake_teardown(&fake);
    home_teardown(&home);
}

MOLTEST(recipe_prefers_the_libstdcxx_an_installed_toolchain_brought) {
    fake_toolchain fake;
    home_fixture home;
    /* The other half of the same rule, and the shape a GCC arrives in: no
       libc++ anywhere, but a libstdc++ of its own that the loader will not find
       until it is told where it is. */
    fake_accepts accepts = { .bare = true, .ships_libstdcxx = true };
    ASSERT_TRUE(fake_setup_installed(&fake, &accepts, &home));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(stdlib_libstdcxx, recipe.stdlib);
    EXPECT_TRUE(has_link_flag(&recipe, "-Wl,-rpath,"));
    /* On the link line only: an rpath on a compile-only command line is an
       unused argument. */
    EXPECT_FALSE(has_compile_flag(&recipe, "-Wl,-rpath,"));
    ASSERT_EQ(1, (int)recipe.runtime_count);
    EXPECT_STREQ(fake.libstdcxx_dir, recipe.runtime_dirs[0]);

    fake_teardown(&fake);
    home_teardown(&home);
}

MOLTEST(recipe_claims_no_library_that_lives_outside_the_prefix) {
    fake_toolchain fake;
    home_fixture home;
    fake_accepts accepts = { .gcc_install = true, .libcxx = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup_installed(&fake, &accepts, &home));

    /* The toolchain is pickup's, and the library it points at is not inside it.
       Ownership is a question about the library, so this one is the host's
       however the compiler reached it — which is exactly the case of an
       installed Clang answering for libstdc++ with a path into /usr. */
    ASSERT_TRUE(fs_format_path(fake.libcxx_dir, sizeof fake.libcxx_dir,
                               "%s/elsewhere", fake.root));
    ASSERT_TRUE(make_library(fake.libcxx_dir, "libc++.so"));
    ASSERT_TRUE(write_driver(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(stdlib_libstdcxx, recipe.stdlib);
    EXPECT_TRUE(has_compile_flag(&recipe, "--gcc-install-dir="));
    EXPECT_EQ(0, (int)recipe.runtime_count);

    fake_teardown(&fake);
    home_teardown(&home);
}

MOLTEST(recipe_orders_a_compiler_the_same_way_without_a_pickup_home) {
    fake_toolchain fake;
    home_fixture home;
    fake_accepts accepts = { .gcc_install = true, .libcxx = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    /* Nowhere to own anything from. The rule has nothing to say, and saying
       nothing has to leave the ordering exactly as it was before it existed. */
    char missing[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(missing, sizeof missing, "%s/no-such-home", fake.root));
    ASSERT_TRUE(home_setup(&home, missing));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(stdlib_libstdcxx, recipe.stdlib);
    EXPECT_TRUE(has_compile_flag(&recipe, "--gcc-install-dir="));

    home_teardown(&home);
    fake_teardown(&fake);
}

MOLTEST(recipe_reports_a_toolchain_that_cannot_build_at_all) {
    fake_toolchain fake;
    fake_accepts accepts = { 0 }; /* accepts nothing */
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    /* An unusable recipe is an answer, not a malfunction: it says this
       compiler cannot build C++ in any configuration Pickup knows to offer. */
    EXPECT_FALSE(recipe.usable);
    EXPECT_EQ(0, (int)recipe.compile_count);
    EXPECT_EQ(0, (int)recipe.link_count);

    fake_teardown(&fake);
}

MOLTEST(recipe_offers_no_libcxx_to_a_toolchain_that_ships_none) {
    fake_toolchain fake;
    /* Every GCC, and every Clang built against the system's library. */
    fake_accepts accepts = { .libcxx = true, .ships_libcxx = false };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    /* It would accept -stdlib=libc++, but there is no library to point at and
       no directory to record, so the candidate is never built. */
    EXPECT_FALSE(recipe.usable);

    fake_teardown(&fake);
}

MOLTEST(recipe_has_nothing_to_discover_without_a_driver) {
    toolchain chain = { 0 };
    link_recipe recipe = recipe_discover(&chain, lang_cxx);
    EXPECT_FALSE(recipe.usable);
    EXPECT_EQ(stdlib_unknown, recipe.stdlib);
}

MOLTEST(recipe_names_every_standard_library_it_can_report) {
    EXPECT_STREQ("libstdc++", recipe_stdlib_name(stdlib_libstdcxx));
    EXPECT_STREQ("libc++", recipe_stdlib_name(stdlib_libcxx));
    EXPECT_STREQ("", recipe_stdlib_name(stdlib_unknown));
}

MOLTEST(recipe_leaves_the_flags_beside_the_driver) {
    fake_toolchain fake;
    fake_accepts accepts = { .libcxx = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);
    ASSERT_TRUE(recipe.usable);
    ASSERT_TRUE(recipe_write_config(fake.driver, &recipe));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(config, sizeof config, "%s.cfg", fake.driver));

    char *written = fs_read_file(config);
    ASSERT_TRUE(written != NULL);
    /* Everything needed to link, one per line, which is the format the driver
       reads. The run-time search path especially: without it the file would
       configure a compiler that links programs which do not start. */
    EXPECT_TRUE(strstr(written, "-stdlib=libc++") != NULL);
    EXPECT_TRUE(strstr(written, "-Wl,-rpath,") != NULL);
    free(written);

    fake_teardown(&fake);
}

MOLTEST(recipe_writes_no_config_for_a_compiler_that_needs_none) {
    fake_toolchain fake;
    fake_accepts accepts = { .bare = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);
    ASSERT_TRUE(recipe.usable);

    /* Nothing to say, so nothing is written: an empty configuration file is
       something for a reader to puzzle over later, not a courtesy. */
    EXPECT_FALSE(recipe_write_config(fake.driver, &recipe));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(config, sizeof config, "%s.cfg", fake.driver));
    EXPECT_FALSE(fs_path_exists(config));

    fake_teardown(&fake);
}

MOLTEST(recipe_describes_the_toolchain_and_not_its_config_file) {
    fake_toolchain fake;
    /* Needs --gcc-install-dir to build. */
    fake_accepts accepts = { .gcc_install = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe first = recipe_discover(&chain, lang_cxx);
    ASSERT_TRUE(first.usable);
    ASSERT_TRUE(recipe_gcc_flag(&first) != NULL);

    /* Now the toolchain is configured, exactly as install leaves it. A driver
       that reads that file builds with no flags at all — so a second discovery
       that let it read the file would publish an empty recipe for a compiler
       that does not build without those flags. It would still work, but only
       for someone invoking that one binary, and a caller handed it would have
       no way to tell. */
    ASSERT_TRUE(recipe_write_config(fake.driver, &first));

    link_recipe second = recipe_discover(&chain, lang_cxx);
    ASSERT_TRUE(second.usable);
    ASSERT_TRUE(recipe_gcc_flag(&second) != NULL);
    EXPECT_STREQ(recipe_gcc_flag(&first), recipe_gcc_flag(&second));

    fake_teardown(&fake);
}

MOLTEST(recipe_puts_both_languages_on_the_same_gcc) {
    fake_toolchain fake;
    /* Accepts everything, so C works untouched while C++ needs the GCC
       pinned — which is the ordinary shape of the problem. */
    fake_accepts accepts = { .bare = true, .gcc_install = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe c = recipe_discover(&chain, lang_c);
    ASSERT_TRUE(c.usable);
    /* C needed nothing, so on its own it names no GCC at all. */
    EXPECT_NULL(recipe_gcc_flag(&c));

    link_recipe cxx = { .usable = true, .stdlib = stdlib_libstdcxx };
    int written = snprintf(cxx.compile_flags[cxx.compile_count++], RECIPE_FLAG_MAX,
                           "--gcc-install-dir=%s", fake.gcc_dir);
    ASSERT_TRUE(written > 0 && written < RECIPE_FLAG_MAX);

    ASSERT_TRUE(recipe_align_gcc(&cxx, fake.driver, &c));

    /* Clang borrows the startup objects and libgcc from that installation too,
       not just libstdc++, so a C driver left choosing its own would have a
       project's C and C++ objects built against two different runtimes. */
    ASSERT_TRUE(recipe_gcc_flag(&c) != NULL);
    EXPECT_TRUE(strstr(recipe_gcc_flag(&c), fake.gcc_dir) != NULL);
    EXPECT_TRUE(has_link_flag(&c, "--gcc-install-dir="));

    fake_teardown(&fake);
}

MOLTEST(recipe_keeps_a_flag_that_would_break_c_out_of_its_recipe) {
    fake_toolchain fake;
    /* Works bare and refuses --gcc-install-dir. C already built without it, so
       adding one that breaks C would trade a mismatch for a compiler that
       builds nothing. */
    fake_accepts accepts = { .bare = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe c = recipe_discover(&chain, lang_c);
    ASSERT_TRUE(c.usable);

    link_recipe cxx = { .usable = true, .stdlib = stdlib_libstdcxx };
    int written = snprintf(cxx.compile_flags[cxx.compile_count++], RECIPE_FLAG_MAX,
                           "--gcc-install-dir=%s", fake.gcc_dir);
    ASSERT_TRUE(written > 0 && written < RECIPE_FLAG_MAX);

    EXPECT_FALSE(recipe_align_gcc(&cxx, fake.driver, &c));
    EXPECT_NULL(recipe_gcc_flag(&c));
    /* And what worked before is untouched. */
    EXPECT_TRUE(c.usable);

    fake_teardown(&fake);
}

MOLTEST(recipe_has_no_gcc_to_align_when_none_was_pinned) {
    link_recipe cxx = { .usable = true };
    link_recipe c = { .usable = true };
    EXPECT_NULL(recipe_gcc_flag(&cxx));
    EXPECT_FALSE(recipe_align_gcc(&cxx, "/nonexistent/cc", &c));
}

MOLTEST(recipe_finds_no_libcxx_where_there_is_none) {
    char directory[PICKUP_PATHS_MAX];
    /* A driver that cannot be run answers nothing, rather than a path that
       would later be written into an executable as a search path. */
    EXPECT_FALSE(recipe_own_libcxx_dir("/nonexistent/pickup/cc",
                                       directory, sizeof directory));
    EXPECT_FALSE(recipe_own_libcxx_dir(NULL, directory, sizeof directory));
}

/*
 * Keeping a configuration file in step with what now works.
 *
 * The file records a decision made at install time and nothing revalidates it,
 * so it can go on naming a GCC that has since stopped being the right one.
 * Rewriting it is the one thing a diagnosis does rather than reports, so what
 * matters most here is what it refuses to touch.
 */

MOLTEST(recipe_refreshes_a_configuration_that_has_fallen_behind) {
    fake_toolchain fake;
    fake_accepts accepts = { .gcc_install = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(config, sizeof config, "%s.cfg", fake.driver));
    ASSERT_TRUE(fs_write_file(config,
        "# Written by pickup: the configuration this toolchain was proven to build with.\n"
        "--gcc-install-dir=/somewhere/that/moved/on\n"));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);
    ASSERT_TRUE(recipe.usable);

    ASSERT_TRUE(recipe_refresh_config(fake.driver, &recipe));

    char *after = fs_read_file(config);
    ASSERT_TRUE(after != NULL);
    EXPECT_TRUE(strstr(after, fake.gcc_dir) != NULL);
    EXPECT_TRUE(strstr(after, "/somewhere/that/moved/on") == NULL);
    free(after);

    fake_teardown(&fake);
}

MOLTEST(recipe_leaves_a_configuration_someone_wrote_by_hand) {
    fake_toolchain fake;
    fake_accepts accepts = { .gcc_install = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    char config[PICKUP_PATHS_MAX];
    ASSERT_TRUE(fs_format_path(config, sizeof config, "%s.cfg", fake.driver));
    /* No pickup header: someone put this here on purpose. A flag added by hand
       is a decision, and overwriting it without asking would undo it
       silently. */
    static const char theirs[] = "--gcc-install-dir=/their/choice\n-DTHEIR_FLAG\n";
    ASSERT_TRUE(fs_write_file(config, theirs));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);
    ASSERT_TRUE(recipe.usable);

    EXPECT_FALSE(recipe_refresh_config(fake.driver, &recipe));

    char *after = fs_read_file(config);
    ASSERT_TRUE(after != NULL);
    EXPECT_STREQ(theirs, after);
    free(after);

    fake_teardown(&fake);
}

MOLTEST(recipe_reports_no_change_when_there_is_none) {
    fake_toolchain fake;
    fake_accepts accepts = { .gcc_install = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);
    ASSERT_TRUE(recipe.usable);
    ASSERT_TRUE(recipe_write_config(fake.driver, &recipe));

    /* Already says exactly this. An identical rewrite would have doctor
       announce a change that did not happen. */
    EXPECT_FALSE(recipe_refresh_config(fake.driver, &recipe));

    fake_teardown(&fake);
}

MOLTEST(recipe_writes_a_configuration_that_was_never_there) {
    fake_toolchain fake;
    fake_accepts accepts = { .gcc_install = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);
    ASSERT_TRUE(recipe.usable);

    /* Nothing to preserve, so there is nothing to refuse. */
    EXPECT_TRUE(recipe_refresh_config(fake.driver, &recipe));

    fake_teardown(&fake);
}

MOLTEST(recipe_names_the_library_a_caller_asked_for) {
    fake_toolchain fake;
    home_fixture home;
    /* Left to itself this toolchain would stand on the libc++ it carries, and
       `install` would write that into a .cfg beside the driver. */
    fake_accepts accepts = { .gcc_install = true, .libcxx = true, .ships_libcxx = true };
    ASSERT_TRUE(fake_setup_installed(&fake, &accepts, &home));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover_for(&chain, lang_cxx, stdlib_libstdcxx, true);

    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(stdlib_libstdcxx, recipe.stdlib);

    /*
     * And it says so out loud.
     *
     * The recipe is discovered with the driver's configuration file ignored,
     * but it is handed to someone who will invoke that driver with the file in
     * place. A recipe that only named a GCC would be overridden by a .cfg
     * saying -stdlib=libc++, and the caller who asked for libstdc++ would get
     * the other one and a link error full of symbols it never mentioned.
     */
    EXPECT_TRUE(has_compile_flag(&recipe, "-stdlib=libstdc++"));
    EXPECT_TRUE(has_link_flag(&recipe, "-stdlib=libstdc++"));

    fake_teardown(&fake);
    home_teardown(&home);
}

MOLTEST(recipe_says_nothing_extra_when_no_library_was_asked_for) {
    fake_toolchain fake;
    fake_accepts accepts = { .bare = true };
    ASSERT_TRUE(fake_setup(&fake, &accepts));

    toolchain chain = chain_of(&fake);
    link_recipe recipe = recipe_discover(&chain, lang_cxx);

    /* No flags remains the most portable answer there is, and naming a library
       nobody asked about would spend that for nothing. */
    ASSERT_TRUE(recipe.usable);
    EXPECT_EQ(0, (int)recipe.compile_count);
    EXPECT_FALSE(has_compile_flag(&recipe, "-stdlib="));

    fake_teardown(&fake);
}

/*
 * A driver whose configuration file is not decoration.
 *
 * llvm-mingw puts `-rtlib=compiler-rt`, `-unwindlib=libunwind` and
 * `-fuse-ld=lld` in a config beside the driver, because without them clang
 * falls back to `-lgcc -lgcc_eh` and that toolchain ships neither. So it parses
 * C perfectly well with its config ignored and fails at the link -- which is
 * exactly the layer `-fsyntax-only` cannot see.
 */
MOLTEST_FAKE(fake_config_dependent_driver) {
    const bool ignoring = argument_given(argc, argv, "--no-default-config");

    if (argument_given(argc, argv, "-fsyntax-only"))
        return 0;

    const char *out = argument_after(argc, argv, "-o");
    if (out == NULL)
        return 0;
    if (ignoring) {
        fprintf(stderr, "lld: error: unable to find library -lgcc\n");
        return 1;
    }
    return moltest_fake_program(out, "exit 0\n", NULL, 0) ? 0 : 1;
}

/*
 * Whether a driver's configuration can be ignored is a question about linking,
 * and it used to be asked with `-fsyntax-only`.
 *
 * The probe passed, `--no-default-config` went into every candidate after it,
 * and the link then reached for libraries the config would have replaced. The
 * toolchain was reported as building nothing at all -- and because the failure
 * happened past `satisfies`, `resolve` printed `missing:` with nothing behind
 * it.
 */
MOLTEST(recipe_keeps_the_configuration_a_driver_needs_to_link) {
    char root[PICKUP_PATHS_MAX];
    ASSERT_TRUE(moltest_temp_dir("pickup_cfg_driver", root, sizeof root));

    char wanted[PICKUP_PATHS_MAX];
    char driver[PICKUP_PATHS_MAX];
    snprintf(wanted, sizeof wanted, "%s/x86_64-w64-mingw32-clang", root);
    ASSERT_TRUE(moltest_fake_program(wanted, "behave fake_config_dependent_driver\n", driver,
                                     sizeof driver));

    toolchain chain = {0};
    chain.vendor = vendor_clang;
    snprintf(chain.name, sizeof chain.name, "x86_64-w64-mingw32-clang");
    snprintf(chain.path, sizeof chain.path, "%s", driver);

    const link_recipe recipe = recipe_discover(&chain, lang_c);
    EXPECT_TRUE(recipe.usable);

    /* And the flag that broke it is not in the answer, because a recipe is
       what makes this toolchain build. */
    for (size_t i = 0; i < recipe.compile_count; i++)
        EXPECT_STRNE("--no-default-config", recipe.compile_flags[i]);
}
