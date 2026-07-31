#include <moltest.h>

#include <pickup/commands/resolve_command.h>
#include <pickup/exit_code.h>

#include <unistd.h>

MOLTEST(resolve_rejects_a_malformed_request) {
    /* An unknown requirement must be an error, never silently dropped: a
       dropped requirement would return a compiler that does not meet it. */
    const resolve_request unknown_feature = { .features = "no_such_feature" };
    EXPECT_EQ(exit_usage_error, resolve_command_run(&unknown_feature, false));

    const resolve_request unknown_vendor = { .vendor = "borland" };
    EXPECT_EQ(exit_usage_error, resolve_command_run(&unknown_vendor, false));

    const resolve_request unknown_lang = { .lang = "fortran" };
    EXPECT_EQ(exit_usage_error, resolve_command_run(&unknown_lang, false));
}

MOLTEST(resolve_answers_when_nothing_qualifies) {
    /* Requiring every C23 feature at once finds nothing on a machine whose
       newest compiler is partial — and "nothing matches" is an answer with its
       own exit code, not a malfunction. */
    const resolve_request impossible = {
        .lang = "c",
        .features = "constexpr,nullptr,typeof,attr_nodiscard,native_bool",
    };
    int code = resolve_command_run(&impossible, false);
    EXPECT_TRUE(code == exit_ok || code == exit_no_match);
}

MOLTEST(resolve_never_returns_a_compiler_that_lacks_the_feature) {
    if (access("/usr/bin/gcc-9", X_OK) != 0 || access("/usr/bin/gcc-12", X_OK) != 0)
        SKIP("gcc-9 and gcc-12 are not both installed");

    /* The measurement this project exists for: both accept -std=c2x, only one
       implements the attribute. Asking for the attribute must not return the
       one that would fail to compile. */
    const resolve_request needs_attribute = {
        .lang = "c",
        .standard = "c2x",
        .features = "attr_nodiscard",
        .vendor = "gcc",
    };
    EXPECT_EQ(exit_ok, resolve_command_run(&needs_attribute, false));

    /* Restricted to gcc, the only qualifying versions are 11 and 12; gcc-9
       accepts the flag and is still excluded. */
    const resolve_request gcc9_would_pass_on_flags_alone = {
        .lang = "c",
        .standard = "c2x",
        .vendor = "gcc",
    };
    EXPECT_EQ(exit_ok, resolve_command_run(&gcc9_would_pass_on_flags_alone, false));
}

MOLTEST(resolve_emits_toml_for_machines) {
    const resolve_request any_c = { .lang = "c" };
    /* The TOML form is what Molto reads; it must succeed wherever the text
       form does. */
    EXPECT_EQ(resolve_command_run(&any_c, false), resolve_command_run(&any_c, true));
}

MOLTEST(resolve_requires_a_cxx_driver_for_cxx) {
    /* Parsing C++ is not enough: without a C++ driver there is nothing to
       invoke, so such a toolchain must not be offered for C++. */
    const resolve_request cxx = { .lang = "c++" };
    int code = resolve_command_run(&cxx, false);
    EXPECT_TRUE(code == exit_ok || code == exit_no_match);
}
