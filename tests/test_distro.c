#include <moltest.h>

#include <pickup/detect/distro.h>

#include <string.h>

/*
 * Recorded os-release files, for the same reason the LLVM tests use a recorded
 * API answer: the mapping has to be checkable from any machine, and the
 * machine running the tests can only ever be one distribution.
 */

static const char ubuntu[] =
    "PRETTY_NAME=\"Ubuntu 22.04.3 LTS\"\n"
    "NAME=\"Ubuntu\"\n"
    "VERSION_ID=\"22.04\"\n"
    "ID=ubuntu\n"
    "ID_LIKE=debian\n"
    "HOME_URL=\"https://www.ubuntu.com/\"\n";

/* A derivative whose own ID Pickup has never heard of. */
static const char pop_os[] =
    "NAME=\"Pop!_OS\"\n"
    "VERSION=\"22.04 LTS\"\n"
    "ID=pop\n"
    "ID_LIKE=\"ubuntu debian\"\n";

static const char fedora[] =
    "NAME=\"Fedora Linux\"\n"
    "VERSION=\"39 (Workstation Edition)\"\n"
    "ID=fedora\n"
    "VERSION_ID=39\n";

static const char arch[] =
    "NAME=\"Arch Linux\"\n"
    "ID=arch\n"
    "BUILD_ID=rolling\n";

static const char alpine[] =
    "NAME=\"Alpine Linux\"\n"
    "ID=alpine\n"
    "VERSION_ID=3.19.1\n";

static const char opensuse[] =
    "NAME=\"openSUSE Tumbleweed\"\n"
    "ID=\"opensuse-tumbleweed\"\n"
    "ID_LIKE=\"opensuse suse\"\n";

MOLTEST(distro_reads_what_the_system_calls_itself) {
    EXPECT_EQ(distro_debian, distro_parse_os_release(ubuntu));
    EXPECT_EQ(distro_fedora, distro_parse_os_release(fedora));
    EXPECT_EQ(distro_arch, distro_parse_os_release(arch));
    EXPECT_EQ(distro_alpine, distro_parse_os_release(alpine));
    EXPECT_EQ(distro_suse, distro_parse_os_release(opensuse));
}

MOLTEST(distro_follows_a_derivative_to_its_parent) {
    /* There are more derivatives than anyone can enumerate, and each one names
       its parent precisely so that this works. */
    EXPECT_EQ(distro_debian, distro_parse_os_release(pop_os));
}

MOLTEST(distro_does_not_mistake_a_field_that_ends_in_the_same_letters) {
    /* VERSION_ID contains ID, and matching it would read a version number as
       the name of a distribution. */
    static const char version_first[] =
        "VERSION_ID=\"22.04\"\n"
        "ID=fedora\n";
    EXPECT_EQ(distro_fedora, distro_parse_os_release(version_first));
}

MOLTEST(distro_admits_when_it_does_not_know) {
    /* An unrecognised system is an answer. What follows from it is that no
       package gets named, which is better than naming the wrong one. */
    static const char nothing_known[] = "ID=plan9\nID_LIKE=inferno\n";
    EXPECT_EQ(distro_unknown, distro_parse_os_release(nothing_known));
    EXPECT_EQ(distro_unknown, distro_parse_os_release(""));
    EXPECT_EQ(distro_unknown, distro_parse_os_release(NULL));
}

MOLTEST(distro_names_the_versioned_package_where_versions_coexist) {
    char package[DISTRO_PACKAGE_MAX];

    /* The version matters: several GCCs are installed side by side and only
       one of them is the one missing its C++ half. */
    ASSERT_TRUE(distro_gxx_package(distro_debian, 12, package, sizeof package));
    EXPECT_STREQ("g++-12", package);

    ASSERT_TRUE(distro_gxx_package(distro_suse, 13, package, sizeof package));
    EXPECT_STREQ("gcc13-c++", package);
}

MOLTEST(distro_names_the_unversioned_package_where_there_is_one) {
    char package[DISTRO_PACKAGE_MAX];

    ASSERT_TRUE(distro_gxx_package(distro_fedora, 13, package, sizeof package));
    EXPECT_STREQ("gcc-c++", package);

    ASSERT_TRUE(distro_gxx_package(distro_alpine, 13, package, sizeof package));
    EXPECT_STREQ("g++", package);
}

MOLTEST(distro_suggests_nothing_where_there_is_nothing_to_install) {
    char package[DISTRO_PACKAGE_MAX];

    /* Arch ships both compilers in one package, so a GCC without C++ is not a
       state it can be in; offering a package name would send a reader looking
       for something that does not exist. */
    EXPECT_FALSE(distro_gxx_package(distro_arch, 13, package, sizeof package));
    EXPECT_FALSE(distro_gxx_package(distro_unknown, 13, package, sizeof package));
}

MOLTEST(distro_needs_a_version_before_naming_a_versioned_package) {
    char package[DISTRO_PACKAGE_MAX];
    /* "g++-0" is not a package anybody can install. */
    EXPECT_FALSE(distro_gxx_package(distro_debian, 0, package, sizeof package));
}

MOLTEST(distro_names_every_family_it_can_report) {
    const distro_family all[] = {
        distro_unknown, distro_debian, distro_fedora,
        distro_arch, distro_suse, distro_alpine,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *name = distro_family_name(all[i]);
        ASSERT_TRUE(name != NULL);
        EXPECT_TRUE(name[0] != '\0');
    }
}
