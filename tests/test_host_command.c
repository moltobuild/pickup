#include <moltest.h>

#include <pickup/commands/host_command.h>
#include <pickup/exit_code.h>
#include <pickup/sources/registry_source.h>

#include <string.h>

/*
 * `pickup host` is the one place the ecosystem's target vocabulary is spoken.
 * Molto asks rather than deriving an answer of its own, so what matters here is
 * that the answer exists, that it is the catalogue's spelling and not the
 * compiler's, and that "nothing runs here" is reported as an answer.
 */

MOLTEST(host_reports_a_target_this_build_can_run) {
    /* Every platform pickup builds on is one the registry names; a host it
       does not name is the branch below, and cannot be reached from a running
       test on a supported platform. */
    const char *target = registry_host_target();

    EXPECT_NOT_NULL(target);
    EXPECT_TRUE(strlen(target) > 0);
    EXPECT_EQ(exit_ok, host_command_run(false));
    EXPECT_EQ(exit_ok, host_command_run(true));
}

MOLTEST(host_speaks_the_catalogue_spelling_not_the_compilers) {
    /* `pickup resolve` answers `x86_64-unknown-linux-gnu`: what a driver emits
       code for. This answers `linux-x86_64`: what the catalogue publishes
       under. Two questions, two strings, and conflating them is what sends a
       client looking for a coordinate nobody published. */
    const char *target = registry_host_target();

    EXPECT_NULL(strstr(target, "unknown"));
    EXPECT_NULL(strstr(target, "-gnu"));
}
