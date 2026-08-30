#include <moltest.h>

#include <pickup/services/diagnostics_service.h>
#include <pickup/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * The claim under test is the organising rule: a finding is a cause, not a
 * symptom. One GCC installed without its C++ half breaks a Clang that selects
 * it and leaves a `g++` that is not there, and those must arrive as one
 * finding with two symptoms rather than two findings.
 *
 * It cannot be tested against the machine running the tests, which may have no
 * broken GCC, or a different one, or none at all. So a machine is built: a
 * directory laid out like a GCC installation with its C++ headers left out,
 * and drivers that behave the way real ones do in front of it.
 */

typedef struct {
    char root[64];
    char gcc_dir[PICKUP_PATHS_MAX];  /* the installation the drivers report */
    char clang[PICKUP_PATHS_MAX];
    char gcc[PICKUP_PATHS_MAX];
} fake_machine;

/* Lay out a GCC installation. With `with_cxx` its libstdc++ headers are
   present, and without them it is the machine this project was written on. */
static bool make_gcc_tree(fake_machine *machine, bool with_cxx) {
    if (!fs_format_path(machine->gcc_dir, sizeof machine->gcc_dir,
                        "%s/usr/lib/gcc/x86_64-linux-gnu/12", machine->root))
        return false;
    if (!fs_make_dirs(machine->gcc_dir))
        return false;
    if (!with_cxx)
        return true;

    char headers[PICKUP_PATHS_MAX];
    if (!fs_format_path(headers, sizeof headers, "%s/usr/include/c++/12",
                        machine->root))
        return false;
    if (!fs_make_dirs(headers))
        return false;

    char iostream[PICKUP_PATHS_MAX];
    if (!fs_format_path(iostream, sizeof iostream, "%s/iostream", headers))
        return false;
    return fs_write_file(iostream, "/* a stand-in */\n");
}

/*
 * A driver that builds C and, when `cxx_works` is false, cannot build C++ —
 * which is exactly what a Clang standing on a GCC without libstdc++ does.
 * Under -v it names the installation it selected, the way a real driver does.
 */
static bool has_argument(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return true;
    }
    return false;
}

static const char *value_after(int argc, char **argv, const char *flag) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    }
    return NULL;
}

static const char *value_of(int argc, char **argv, const char *prefix) {
    const size_t width = strlen(prefix);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], prefix, width) == 0)
            return argv[i] + width;
    }
    return NULL;
}

static bool says_yes(const char *key) {
    const char *value = moltest_fake_setting(key);
    return value != NULL && strcmp(value, "yes") == 0;
}

/* A driver written in C rather than as a shell script, because a fake has to be
   a program the platform can start. What varies between fixtures — whether the
   C++ side works, whether it reports a GCC — travels in the spec. */
MOLTEST_FAKE(diagnostics_driver) {
    const char *asked = value_of(argc, argv, "-print-file-name=");
    if (asked != NULL) {
        printf("%s\n", asked);
        return 0;
    }
    if (has_argument(argc, argv, "-dumpmachine")) {
        printf("x86_64-linux-gnu\n");
        return 0;
    }
    if (has_argument(argc, argv, "-v")) {
        if (says_yes("report_gcc")) {
            const char *dir = moltest_fake_setting("gcc_dir");
            fprintf(stderr, "Selected GCC installation: %s\n", dir != NULL ? dir : "");
        }
        return 0;
    }

    const char *language = value_after(argc, argv, "-x");
    if (language != NULL && strcmp(language, "c++") == 0 && !says_yes("cxx_works"))
        return 1;
    if (has_argument(argc, argv, "-fsyntax-only"))
        return 0;

    const char *out = value_after(argc, argv, "-o");
    if (out != NULL && !moltest_fake_program(out, "exit 0\n", NULL, 0))
        return 1;
    return 0;
}

static bool write_driver(const char *path, const char *gcc_dir, bool cxx_works, bool report_gcc) {
    char spec[512];
    const int written = snprintf(spec, sizeof spec,
                                 "set cxx_works %s\n"
                                 "set report_gcc %s\n"
                                 "set gcc_dir %s\n"
                                 "behave diagnostics_driver\n",
                                 cxx_works ? "yes" : "no", report_gcc ? "yes" : "no", gcc_dir);
    if (written < 0 || (size_t)written >= sizeof spec)
        return false;
    return moltest_fake_program(path, spec, NULL, 0);
}

static bool machine_setup(fake_machine *machine, bool gcc_has_cxx) {
    if (!moltest_temp_dir("pickup_diag", machine->root, sizeof machine->root))
        return false;
    if (!make_gcc_tree(machine, gcc_has_cxx))
        return false;

    /* The GCC sits under the same prefix as the installation it belongs to —
       /usr/bin/gcc-12 beside /usr/lib/gcc/…/12 — because that is what makes it
       the same GCC. A compiler of the same version somewhere else is a
       different one, and must not be blamed for this one's fault. */
    char bin[PICKUP_PATHS_MAX];
    if (!fs_format_path(bin, sizeof bin, "%s/usr/bin", machine->root)
        || !fs_make_dirs(bin))
        return false;
    if (!fs_format_path(machine->clang, sizeof machine->clang, "%s/clang",
                        machine->root)
        || !fs_format_path(machine->gcc, sizeof machine->gcc, "%s/gcc-12", bin))
        return false;

    /* The Clang builds C++ only where the installation it selects is whole. */
    return write_driver(machine->clang, machine->gcc_dir, gcc_has_cxx, true)
        && write_driver(machine->gcc, machine->gcc_dir, false, false);
}

static void machine_teardown(fake_machine *machine) {
    (void)fs_remove_tree(machine->root);
}

/* Add a toolchain to an inventory. `cxx` empty means it has no C++ driver. */
static bool add_chain(inventory *list, const char *name, const char *path,
                      const char *cxx, toolchain_vendor vendor, int major) {
    toolchain chain = { 0 };
    snprintf(chain.name, sizeof chain.name, "%s", name);
    snprintf(chain.path, sizeof chain.path, "%s", path);
    snprintf(chain.cxx_path, sizeof chain.cxx_path, "%s", cxx);
    chain.vendor = vendor;
    chain.version = (toolchain_version){ .major = major };
    return inventory_append(list, &chain);
}

/* Count the findings whose subject starts with `prefix`. */
static int findings_about(const diagnostics_report *report, const char *prefix) {
    int count = 0;
    for (size_t i = 0; i < report->count; i++) {
        if (strncmp(report->items[i].subject, prefix, strlen(prefix)) == 0)
            count++;
    }
    return count;
}

MOLTEST(diagnostics_reports_one_cause_rather_than_every_symptom) {
    fake_machine machine;
    ASSERT_TRUE(machine_setup(&machine, false));

    inventory list = { 0 };
    /* The Clang that selects the incomplete GCC, and the GCC of that version
       with no C++ driver of its own. Two broken things, one reason. */
    ASSERT_TRUE(add_chain(&list, "clang", machine.clang, machine.clang,
                          vendor_clang, 14));
    ASSERT_TRUE(add_chain(&list, "gcc-12", machine.gcc, "", vendor_gcc, 12));

    inventory_settle(&list);
    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));

    /* One finding, and it is about the GCC installation rather than about
       either compiler that suffers from it. */
    ASSERT_EQ(1, (int)report.count);
    EXPECT_STREQ("gcc 12", report.items[0].subject);
    EXPECT_STREQ(machine.gcc_dir, report.items[0].location);
    EXPECT_EQ(finding_error, report.items[0].severity);

    /* Both compilers appear, as symptoms of it. */
    ASSERT_EQ(2, (int)report.items[0].symptom_count);
    EXPECT_TRUE(strstr(report.items[0].symptoms[0], "clang") != NULL);
    EXPECT_TRUE(strstr(report.items[0].symptoms[0], "<iostream>") != NULL);
    EXPECT_TRUE(strstr(report.items[0].symptoms[1], "gcc@12") != NULL);

    inventory_free(&list);
    machine_teardown(&machine);
}

MOLTEST(diagnostics_blames_only_the_compilers_of_the_broken_installation) {
    fake_machine machine;
    ASSERT_TRUE(machine_setup(&machine, false));

    inventory list = { 0 };
    ASSERT_TRUE(add_chain(&list, "clang", machine.clang, machine.clang,
                          vendor_clang, 14));
    /* The system's GCC 12, which does belong to the broken installation. */
    ASSERT_TRUE(add_chain(&list, "gcc-12", machine.gcc, "", vendor_gcc, 12));
    /* And a GCC 12 living somewhere else entirely — one Pickup installed under
       its own home, say. Same version number, different installation, and
       nothing to do with this fault. */
    ASSERT_TRUE(add_chain(&list, "elsewhere", "/opt/other/bin/gcc", "",
                          vendor_gcc, 12));

    inventory_settle(&list);
    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));

    const finding *installation = NULL;
    for (size_t i = 0; i < report.count && installation == NULL; i++) {
        if (strcmp(report.items[i].subject, "gcc 12") == 0)
            installation = &report.items[i];
    }
    ASSERT_TRUE(installation != NULL);

    /* Two symptoms, not three: sharing a version number is not what makes a
       compiler part of an installation. The one living elsewhere may well have
       problems of its own, but they are not this finding's. */
    EXPECT_EQ(2, (int)installation->symptom_count);
    for (size_t i = 0; i < installation->symptom_count; i++)
        EXPECT_TRUE(strstr(installation->symptoms[i], "elsewhere") == NULL);

    inventory_free(&list);
    machine_teardown(&machine);
}

MOLTEST(diagnostics_names_the_package_of_the_distribution_it_is_on) {
    fake_machine machine;
    ASSERT_TRUE(machine_setup(&machine, false));

    inventory list = { 0 };
    ASSERT_TRUE(add_chain(&list, "clang", machine.clang, machine.clang,
                          vendor_clang, 14));

    inventory_settle(&list);
    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));
    ASSERT_EQ(1, (int)report.count);

    /* Two remedies, and they do different things, so both are offered. */
    ASSERT_EQ(2, (int)report.items[0].remedy_count);
    EXPECT_TRUE(strstr(report.items[0].remedies[0], "g++-12") != NULL);
    EXPECT_TRUE(strstr(report.items[0].remedies[1], "pickup install gcc") != NULL);
    /* The second must not read as an equivalent of the first: it leaves the
       broken installation exactly where it was. */
    EXPECT_TRUE(strstr(report.items[0].remedies[1], "untouched") != NULL);

    inventory_free(&list);
    machine_teardown(&machine);
}

MOLTEST(diagnostics_suggests_no_package_it_cannot_name) {
    fake_machine machine;
    ASSERT_TRUE(machine_setup(&machine, false));

    inventory list = { 0 };
    ASSERT_TRUE(add_chain(&list, "clang", machine.clang, machine.clang,
                          vendor_clang, 14));

    inventory_settle(&list);
    diagnostics_report report = { 0 };
    /* On a distribution Pickup does not recognise the package name is
       unknown, and inventing one would send a reader looking for something
       that does not exist. What pickup can do itself still stands. */
    ASSERT_TRUE(diagnostics_examine(&list, distro_unknown, &report));
    ASSERT_EQ(1, (int)report.count);
    ASSERT_EQ(1, (int)report.items[0].remedy_count);
    EXPECT_TRUE(strstr(report.items[0].remedies[0], "pickup install gcc") != NULL);

    inventory_free(&list);
    machine_teardown(&machine);
}

MOLTEST(diagnostics_finds_nothing_wrong_with_a_whole_installation) {
    fake_machine machine;
    ASSERT_TRUE(machine_setup(&machine, true));

    inventory list = { 0 };
    ASSERT_TRUE(add_chain(&list, "clang", machine.clang, machine.clang,
                          vendor_clang, 14));

    inventory_settle(&list);
    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));

    /* A machine that builds is a machine with nothing to report. An empty
       report is the result, not a failure to produce one. */
    EXPECT_EQ(0, (int)report.count);
    EXPECT_FALSE(diagnostics_has_errors(&report));

    inventory_free(&list);
    machine_teardown(&machine);
}

MOLTEST(diagnostics_does_not_fault_a_c_compiler_for_having_no_cxx) {
    fake_machine machine;
    ASSERT_TRUE(machine_setup(&machine, true));

    inventory list = { 0 };
    /* A C compiler with no C++ driver is the ordinary state of `cc`, not a
       fault, and reporting it would bury the real findings in noise. */
    ASSERT_TRUE(add_chain(&list, "cc", machine.gcc, "", vendor_gcc, 9));

    inventory_settle(&list);
    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));
    EXPECT_EQ(0, (int)findings_about(&report, "cc"));

    inventory_free(&list);
    machine_teardown(&machine);
}

MOLTEST(diagnostics_reports_a_machine_with_no_compilers) {
    inventory list = { 0 };
    inventory_settle(&list);
    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));

    ASSERT_EQ(1, (int)report.count);
    EXPECT_EQ(finding_error, report.items[0].severity);
    EXPECT_TRUE(report.items[0].remedy_count > 0);

    inventory_free(&list);
}

MOLTEST(diagnostics_separates_what_stops_a_build_from_what_merely_annoys) {
    diagnostics_report report = { 0 };
    EXPECT_FALSE(diagnostics_has_errors(&report));

    report.items[0].severity = finding_warning;
    report.count = 1;
    /* A warning leaves the exit code at zero: the machine can still build. */
    EXPECT_FALSE(diagnostics_has_errors(&report));

    report.items[0].severity = finding_error;
    EXPECT_TRUE(diagnostics_has_errors(&report));
}

MOLTEST(diagnostics_names_every_severity) {
    const finding_severity all[] = { finding_ok, finding_warning, finding_error };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *name = finding_severity_name(all[i]);
        ASSERT_TRUE(name != NULL);
        EXPECT_TRUE(name[0] != '\0');
    }
}

/*
 * Severity by impact.
 *
 * The rule the whole report is built on: a thing is a failure when it stops
 * someone, not when it is imperfect. The same broken GCC is blocking on a
 * machine where nothing else builds C++ and merely true on one where three
 * other toolchains do.
 */

MOLTEST(diagnostics_does_not_fail_over_what_something_else_covers) {
    fake_machine broken;
    fake_machine whole;
    ASSERT_TRUE(machine_setup(&broken, false));
    ASSERT_TRUE(machine_setup(&whole, true));

    inventory list = { 0 };
    /* One clang standing on the incomplete GCC, and another that builds C++
       perfectly well. The first is still broken; nobody is stopped. */
    ASSERT_TRUE(add_chain(&list, "clang", broken.clang, broken.clang,
                          vendor_clang, 14));
    ASSERT_TRUE(add_chain(&list, "clang", whole.clang, whole.clang,
                          vendor_clang, 22));
    inventory_settle(&list);

    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));

    const finding *installation = NULL;
    for (size_t i = 0; i < report.count && installation == NULL; i++) {
        if (report.items[i].section == section_compilers)
            installation = &report.items[i];
    }
    ASSERT_TRUE(installation != NULL);

    /* Reported, and not as a failure. That is what keeps the exit code
       meaning "this machine cannot be worked on". */
    EXPECT_FALSE(installation->blocking);
    EXPECT_FALSE(diagnostics_is_blocked(&report));

    inventory_free(&list);
    machine_teardown(&broken);
    machine_teardown(&whole);
}

MOLTEST(diagnostics_fails_when_nothing_covers_it) {
    fake_machine machine;
    ASSERT_TRUE(machine_setup(&machine, false));

    inventory list = { 0 };
    /* The same fault, alone. Now there is no C++ on this machine at all. */
    ASSERT_TRUE(add_chain(&list, "clang", machine.clang, machine.clang,
                          vendor_clang, 14));
    inventory_settle(&list);

    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));

    EXPECT_TRUE(diagnostics_is_blocked(&report));
    EXPECT_EQ(1, findings_about(&report, "gcc 12"));

    inventory_free(&list);
    machine_teardown(&machine);
}

MOLTEST(diagnostics_says_what_the_machine_has_even_when_all_is_well) {
    fake_machine machine;
    ASSERT_TRUE(machine_setup(&machine, true));

    inventory list = { 0 };
    ASSERT_TRUE(add_chain(&list, "clang", machine.clang, machine.clang,
                          vendor_clang, 22));
    inventory_settle(&list);

    diagnostics_report report = { 0 };
    ASSERT_TRUE(diagnostics_examine(&list, distro_debian, &report));

    /* "What is broken" is the question asked least often. A report with
       nothing to complain about still has to answer "what does this machine
       have", or it says nothing at all. */
    ASSERT_TRUE(report.summary_count >= 2);
    bool counted = false, ranged = false;
    for (size_t i = 0; i < report.summary_count; i++) {
        if (report.summary_section[i] != section_compilers)
            continue;
        ranged = ranged || strstr(report.summary[i], "clang") != NULL;
        counted = counted || strstr(report.summary[i], "build C and C++") != NULL;
    }
    EXPECT_TRUE(ranged);
    EXPECT_TRUE(counted);

    inventory_free(&list);
    machine_teardown(&machine);
}

MOLTEST(diagnostics_names_every_section) {
    const finding_section all[] = {
        section_compilers, section_tools, section_environment,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *name = finding_section_name(all[i]);
        ASSERT_TRUE(name != NULL);
        EXPECT_TRUE(name[0] != '\0');
    }
}
