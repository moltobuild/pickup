#include <moltest.h>

#include <pickup/commands/resolve_command.h>
#include <pickup/services/inventory_service.h>
#include <pickup/services/paths_service.h>
#include <pickup/exit_code.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
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

/* Run `resolve` with stderr captured, and hand back what it said. */
static bool resolve_saying(const resolve_request *request, int *code, char *text, size_t size) {
    char path[PICKUP_PATHS_MAX];
    if (!moltest_temp_file("pickup_resolve_say", path, sizeof path))
        return false;
    const int fd = open(path, O_WRONLY);
    if (fd < 0)
        return false;

    const int saved = dup(STDERR_FILENO);
    if (saved < 0 || (fflush(stderr), dup2(fd, STDERR_FILENO)) < 0) {
        (void)close(fd);
        return false;
    }
    *code = resolve_command_run(request, false);
    fflush(stderr);
    (void)dup2(saved, STDERR_FILENO);
    (void)close(saved);
    (void)close(fd);

    FILE *written = fopen(path, "r");
    if (written == NULL)
        return false;
    const size_t read = fread(text, 1, size - 1, written);
    text[read] = '\0';
    (void)fclose(written);
    (void)remove(path);
    return true;
}

/*
 * A target names the platform the code is for, and a machine has compilers for
 * exactly one of them. Asking for another is the ordinary way a cross request
 * fails, and it has to say so: rejected for what the candidate *is* rather than
 * for something it lacks, which is the same shape as the vendor below.
 */
MOLTEST(resolve_says_the_target_when_the_target_is_what_did_not_match) {
    const resolve_request elsewhere = {.lang = "c", .target = "sparc-unknown-none-elf"};
    int code = 0;
    char text[8192] = "";
    ASSERT_TRUE(resolve_saying(&elsewhere, &code, text, sizeof text));

    EXPECT_EQ(exit_no_match, code);
    EXPECT_NOT_NULL(strstr(text, "target sparc-unknown-none-elf"));
    /* And it names what the candidate does emit for, so the reader can see how
       far off the ask was. */
    EXPECT_NOT_NULL(strstr(text, "it emits for"));
    EXPECT_NULL(strstr(text, "missing:\n"));
}

/* The target this machine's compilers actually emit for is accepted, spelled
   the way a compiler reports it. Read back from the answer rather than assumed,
   because the triple differs between distributions. */
MOLTEST(resolve_accepts_the_target_its_own_compilers_report) {
    const resolve_request here = {.lang = "c"};
    int code = 0;
    char text[8192] = "";
    ASSERT_TRUE(resolve_saying(&here, &code, text, sizeof text));
    ASSERT_EQ(exit_ok, code);

    inventory list;
    ASSERT_TRUE(inventory_load(&list, false));
    ASSERT_TRUE(list.count > 0);

    char target[PICKUP_TARGET_MAX];
    snprintf(target, sizeof target, "%s", list.items[0].target);
    inventory_free(&list);

    /* Whatever it is, something answers for it. */
    const resolve_request by_triple = {.lang = "c", .target = target};
    ASSERT_TRUE(resolve_saying(&by_triple, &code, text, sizeof text));
    EXPECT_EQ(exit_ok, code);
}

/* The bug this closes: a candidate rejected for its vendor was listed with an
   empty `missing:`, because the reason was walked out of a feature catalogue
   that cannot hold it. Seven compilers, seven blank lines, and no diagnosis. */
MOLTEST(resolve_says_the_vendor_when_the_vendor_is_what_did_not_match) {
    char path[PICKUP_PATHS_MAX];
    ASSERT_TRUE(moltest_temp_file("pickup_resolve_err", path, sizeof path));

    /* Opened rather than handed back by the temporary: this one is redirected
       onto stderr, so it needs a descriptor and not just a name. */
    const int fd = open(path, O_WRONLY);
    ASSERT_TRUE(fd >= 0);

    int saved = dup(STDERR_FILENO);
    ASSERT_TRUE(saved >= 0);
    fflush(stderr);
    ASSERT_TRUE(dup2(fd, STDERR_FILENO) >= 0);

    /* Every machine has some C compiler and none of them is MSVC, so every
       candidate is rejected and rejected for this reason. */
    const resolve_request wrong_vendor = { .lang = "c", .vendor = "msvc" };
    const int code = resolve_command_run(&wrong_vendor, false);

    fflush(stderr);
    (void)dup2(saved, STDERR_FILENO);
    (void)close(saved);
    (void)close(fd);

    FILE *written = fopen(path, "r");
    ASSERT_NOT_NULL(written);
    char text[8192] = "";
    const size_t read = fread(text, 1, sizeof text - 1, written);
    text[read] = '\0';
    fclose(written);
    (void)remove(path);

    EXPECT_EQ(exit_no_match, code);
    EXPECT_NOT_NULL(strstr(text, "vendor msvc"));
    /* And no line is left saying nothing. */
    EXPECT_NULL(strstr(text, "missing:\n"));
}
