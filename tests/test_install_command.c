#include <moltest.h>

#include <pickup/commands/install_command.h>
#include <pickup/exit_code.h>

/*
 * These cover only the paths install_command_run resolves before it ever
 * reaches the registry, since anything past that talks to the network and has
 * no place in a unit test.
 */

MOLTEST(install_rejects_a_name_and_version_that_disagree) {
    /* "clang@22.1.0 --version 19" names the version twice; picking one over
       the other silently would install something other than what was asked
       for. */
    const install_command_request request = {
        .name = "clang@22.1.0",
        .version = "19",
    };
    EXPECT_EQ(exit_usage_error, install_command_run(&request));
}

MOLTEST(install_rejects_a_name_and_version_even_when_they_agree) {
    /* Repeating the same version does not make the two spellings one
       request; it is still two answers to the same question. */
    const install_command_request request = {
        .name = "clang@22.1.0",
        .version = "22.1.0",
    };
    EXPECT_EQ(exit_usage_error, install_command_run(&request));
}

MOLTEST(install_rejects_a_versioned_name_with_nothing_before_the_at) {
    /* "@22.1.0" splits into an empty name, which is not one the registry
       could ever publish. */
    const install_command_request request = {.name = "@22.1.0"};
    EXPECT_EQ(exit_usage_error, install_command_run(&request));
}

MOLTEST(install_leaves_an_unversioned_name_alone) {
    /* No '@' at all is the common case, and must reach the usual name
       validation unmodified rather than being rejected for carrying a
       version it never named. */
    const install_command_request malformed = {.name = "../etc"};
    EXPECT_EQ(exit_usage_error, install_command_run(&malformed));
}
