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
    char driver[PICKUP_PATHS_MAX];
    char gcc_dir[PICKUP_PATHS_MAX];   /* the GCC installation it reports */
    char libcxx_dir[PICKUP_PATHS_MAX];/* where its own libc++ sits */
} fake_toolchain;

/* What a fake is willing to accept. */
typedef struct {
    bool bare;           /* works with no flags at all */
    bool gcc_install;    /* works when given --gcc-install-dir= */
    bool libcxx;         /* works when given -stdlib=libc++ */
    bool ships_libcxx;   /* carries a libc++ to be pointed at */
} fake_accepts;

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

static bool make_libcxx(fake_toolchain *fake) {
    if (!fs_format_path(fake->libcxx_dir, sizeof fake->libcxx_dir,
                        "%s/lib", fake->root))
        return false;
    if (!fs_make_dirs(fake->libcxx_dir))
        return false;

    char library[PICKUP_PATHS_MAX];
    if (!fs_format_path(library, sizeof library, "%s/libc++.so", fake->libcxx_dir))
        return false;
    return fs_write_file(library, "/* a stand-in */\n");
}

/* Write a driver that behaves as `accepts` says. */
static bool write_driver(const fake_toolchain *fake, const fake_accepts *accepts) {
    char script[8192];
    int written = snprintf(script, sizeof script,
        "#!/bin/sh\n"
        "out=''\n"
        "syntax=no\n"
        "verbose=no\n"
        "gccinstall=no\n"
        "libcxx=no\n"
        "printname=''\n"
        "for a in \"$@\"; do\n"
        "  case \"$a\" in\n"
        "    -v) verbose=yes ;;\n"
        "    -fsyntax-only) syntax=yes ;;\n"
        "    --gcc-install-dir=*) gccinstall=yes ;;\n"
        "    -stdlib=libc++) libcxx=yes ;;\n"
        "    -print-file-name=*) printname=\"${a#-print-file-name=}\" ;;\n"
        "  esac\n"
        "done\n"
        /* -o takes its value in the next argument, so it needs a real walk. */
        "while [ $# -gt 0 ]; do\n"
        "  [ \"$1\" = '-o' ] && { shift; out=\"$1\"; }\n"
        "  shift\n"
        "done\n"
        "if [ -n \"$printname\" ]; then\n"
        "  if [ \"$printname\" = 'libc++.so' ] && [ '%s' = 'yes' ]; then\n"
        "    echo '%s/libc++.so'\n"
        "  else\n"
        "    echo \"$printname\"\n"
        "  fi\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$verbose\" = yes ]; then\n"
        "  echo 'Found candidate GCC installation: %s' >&2\n"
        "  echo 'Selected GCC installation: %s' >&2\n"
        "  exit 0\n"
        "fi\n"
        "src=$(cat)\n"
        /* Whether this invocation is one the driver is willing to serve. */
        "ok=no\n"
        "[ \"$gccinstall\" = yes ] && [ '%s' = yes ] && ok=yes\n"
        "[ \"$libcxx\" = yes ] && [ '%s' = yes ] && ok=yes\n"
        "[ \"$gccinstall\" = no ] && [ \"$libcxx\" = no ] && [ '%s' = yes ] && ok=yes\n"
        "[ \"$ok\" = yes ] || exit 1\n"
        /* A program asking which standard library this is gets answered the
           way a real one would: by failing to compile against the other. Only
           -stdlib=libc++ reaches libc++ here, as on a real Linux driver. */
        "case \"$src\" in\n"
        "  *_LIBCPP_VERSION*) [ \"$libcxx\" = yes ] || exit 1 ;;\n"
        "  *__GLIBCXX__*)     [ \"$libcxx\" = no ] || exit 1 ;;\n"
        "esac\n"
        "[ \"$syntax\" = yes ] && exit 0\n"
        "printf '#!/bin/sh\\nexit 0\\n' > \"$out\"\n"
        "chmod 700 \"$out\"\n"
        "exit 0\n",
        accepts->ships_libcxx ? "yes" : "no", fake->libcxx_dir,
        fake->gcc_dir, fake->gcc_dir,
        accepts->gcc_install ? "yes" : "no",
        accepts->libcxx ? "yes" : "no",
        accepts->bare ? "yes" : "no");

    if (written < 0 || (size_t)written >= sizeof script)
        return false;
    if (!fs_write_file(fake->driver, script))
        return false;
    return chmod(fake->driver, 0700) == 0;
}

static bool fake_setup(fake_toolchain *fake, const fake_accepts *accepts) {
    snprintf(fake->root, sizeof fake->root, "%s", "/tmp/pickup_recipe_XXXXXX");
    if (mkdtemp(fake->root) == NULL)
        return false;
    if (!fs_format_path(fake->driver, sizeof fake->driver, "%s/fakecc", fake->root))
        return false;
    if (!make_gcc_tree(fake) || !make_libcxx(fake))
        return false;
    return write_driver(fake, accepts);
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

MOLTEST(recipe_prefers_the_system_library_when_both_would_work) {
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
