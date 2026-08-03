#include <pickup/detect/health.h>

#include <pickup/services/process_service.h>

#include <stdlib.h>
#include <unistd.h>

/* Compiler arguments. */
#define ARG_LANG        "-x"
#define LANG_C_NAME     "c"
#define LANG_CXX_NAME   "c++"
#define ARG_STDIN       "-"
#define ARG_SYNTAX_ONLY "-fsyntax-only"
#define ARG_OUTPUT      "-o"

/* Room for the composed command line: the driver, the language, the caller's
   flags, the output and the terminator. */
#define HEALTH_MAX_ARGS 32

/* Where the linked program is written before being run. It has to be a real
   file rather than /dev/null: the whole point is to start it afterwards. */
#define TEMPLATE_PATH "/tmp/pickup_health_XXXXXX"

/* What a process reports when the binary could not be executed. */
#define EXIT_NOT_RUNNABLE 127

/*
 * The programs. Each includes the header a real translation unit would, which
 * is what the capability catalog deliberately does not do, and prints through
 * it so that the standard library is linked and not merely parsed.
 */
static const char program_c[] =
    "#include <stdio.h>\n"
    "int main(void){ puts(\"ok\"); return 0; }\n";

static const char program_cxx[] =
    "#include <iostream>\n"
    "int main(){ std::cout << \"ok\\n\"; return 0; }\n";

const char *health_status_message(health_status status) {
    switch (status) {
    case health_ok:         return "builds";
    case health_no_driver:  return "no driver";
    case health_no_headers: return "the standard header does not resolve";
    case health_no_link:    return "compiles but does not link";
    case health_no_run:     return "links but the executable does not start";
    }
    return "unknown";
}

bool health_is_usable(health_status status) {
    return status == health_ok;
}

/* Lay out the fixed head of a command line, returning how many slots it used.
   Every invocation starts the same way and differs only in what follows. */
static size_t begin_command(const char *argv[], const char *compiler,
                            capability_lang lang, const char *const *flags,
                            size_t flag_count) {
    size_t count = 0;
    argv[count++] = compiler;
    argv[count++] = ARG_LANG;
    argv[count++] = lang == lang_c ? LANG_C_NAME : LANG_CXX_NAME;

    /* Bounded so a caller with an unreasonable number of flags truncates
       rather than writing past the array. */
    for (size_t i = 0; i < flag_count && count + 4 < HEALTH_MAX_ARGS; i++)
        argv[count++] = flags[i];
    return count;
}

/* Run the syntax-only pass. Reported as three outcomes rather than two,
   because a driver that could not be executed at all must not be described as
   one whose headers are missing: the first is an absent compiler, the second a
   present compiler with an incomplete standard library. */
typedef enum { compile_ok, compile_rejected, compile_not_runnable } compile_outcome;

static compile_outcome compiles(const char *compiler, capability_lang lang,
                                const char *program, const char *const *flags,
                                size_t flag_count) {
    const char *argv[HEALTH_MAX_ARGS];
    size_t count = begin_command(argv, compiler, lang, flags, flag_count);
    argv[count++] = ARG_SYNTAX_ONLY;
    argv[count++] = ARG_STDIN;
    argv[count] = NULL;

    process_result result = process_try(argv, program);
    if (!result.completed || result.exit_code == EXIT_NOT_RUNNABLE)
        return compile_not_runnable;
    return result.exit_code == 0 ? compile_ok : compile_rejected;
}

/* True if the program links into `output`. */
static bool links(const char *compiler, capability_lang lang, const char *program,
                  const char *const *flags, size_t flag_count, const char *output) {
    const char *argv[HEALTH_MAX_ARGS];
    size_t count = begin_command(argv, compiler, lang, flags, flag_count);
    argv[count++] = ARG_OUTPUT;
    argv[count++] = output;
    argv[count++] = ARG_STDIN;
    argv[count] = NULL;

    process_result result = process_try(argv, program);
    return result.completed && result.exit_code == 0;
}

/* True if the linked program starts and exits cleanly.

   Nothing else Pickup does runs what a compiler produced, and nothing else
   would notice a toolchain whose libraries the loader cannot find. */
static bool runs(const char *path) {
    const char *argv[] = { path, NULL };
    process_result result = process_try(argv, NULL);
    return result.completed && result.exit_code == 0;
}

health_status health_probe(const char *compiler, capability_lang lang,
                           const char *const *flags, size_t flag_count) {
    if (compiler == NULL || compiler[0] == '\0')
        return health_no_driver;

    const char *program = lang == lang_c ? program_c : program_cxx;
    switch (compiles(compiler, lang, program, flags, flag_count)) {
    case compile_not_runnable: return health_no_driver;
    case compile_rejected:     return health_no_headers;
    case compile_ok:           break;
    }

    char output[] = TEMPLATE_PATH;
    int fd = mkstemp(output);
    if (fd < 0)
        return health_no_link; /* nowhere to link to; not a claim of success */
    close(fd);

    health_status status = health_ok;
    if (!links(compiler, lang, program, flags, flag_count, output))
        status = health_no_link;
    else if (!runs(output))
        status = health_no_run;

    /* Removed however it went: a probe must not leave anything behind. */
    (void)unlink(output);
    return status;
}

toolchain_health health_check(const toolchain *chain) {
    toolchain_health health = {
        .c = health_probe(chain->path, lang_c, NULL, 0),
        .cxx = health_no_driver,
    };
    /* Probed with the C++ driver: the C one cannot link a C++ program, so
       asking it would report a failure that belongs to nobody. */
    if (chain->cxx_path[0] != '\0')
        health.cxx = health_probe(chain->cxx_path, lang_cxx, NULL, 0);
    return health;
}
