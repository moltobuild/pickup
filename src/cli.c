#include <pickup/cli.h>

#include <pickup/commands/list_command.h>
#include <pickup/commands/resolve_command.h>
#include <pickup/commands/scan_command.h>
#include <pickup/commands/show_command.h>
#include <pickup/exit_code.h>
#include <pickup/util/cli.h>

#include <stdio.h>
#include <string.h>

#define PICKUP_VERSION "0.1.0"

/* Machine-readable output is TOML because Molto already parses TOML: consuming
   pickup adds no parser to the consumer (spec.md section 13). */
#define FORMAT_TOML "toml"
#define FORMAT_TEXT "text"

const char *cli_version(void) {
    return PICKUP_VERSION;
}

/* The --format option shared by every command that produces data. */
static const cli_option format_option[] = {
    { "--format", 'f', cli_opt_value, "<text|toml>",
      "Output format", FORMAT_TEXT },
};

static bool wants_toml(const cli_args *args) {
    const char *format = cli_args_option(args, "--format");
    return format != NULL && strcmp(format, FORMAT_TOML) == 0;
}

/* What `pickup resolve` accepts. A request states what the caller needs; none
   of it names a machine. */
static const cli_option resolve_options[] = {
    { "--lang", 'l', cli_opt_value, "<c|c++>", "Language to compile", "c" },
    { "--std", 's', cli_opt_value, "<name>", "Standard flag the compiler must accept", NULL },
    { "--require", 'r', cli_opt_value, "<ids>",
      "Comma-separated features that must be present", NULL },
    { "--vendor", 'v', cli_opt_value, "<name>", "Restrict to one vendor", NULL },
    { "--format", 'f', cli_opt_value, "<text|toml>", "Output format", FORMAT_TEXT },
};

/* --- command handlers --- */

static int handle_list(const cli_args *args) {
    return list_command_run(wants_toml(args));
}

static int handle_show(const cli_args *args) {
    return show_command_run(cli_args_positional(args, 0), wants_toml(args));
}

static int handle_resolve(const cli_args *args) {
    const resolve_request request = {
        .lang = cli_args_option(args, "--lang"),
        .standard = cli_args_option(args, "--std"),
        .features = cli_args_option(args, "--require"),
        .vendor = cli_args_option(args, "--vendor"),
    };
    return resolve_command_run(&request, wants_toml(args));
}

static int handle_scan(const cli_args *args) {
    (void)args;
    return scan_command_run();
}

static int handle_unimplemented(const cli_args *args) {
    fprintf(stderr, "pickup: '%s' is not implemented yet (see spec.md)\n",
            cli_args_command_name(args));
    return exit_not_implemented;
}

/* --- command table --- */

static const cli_command commands[] = {
    { "list", "List the compilers found on this machine", NULL,
      format_option, sizeof format_option / sizeof format_option[0], handle_list },
    { "show", "Show one toolchain in detail, feature by feature", "<name>",
      format_option, sizeof format_option / sizeof format_option[0], handle_show },
    { "scan", "Probe every compiler again and rewrite the cache", NULL,
      NULL, 0, handle_scan },
    { "resolve", "Find the best toolchain for a set of requirements", NULL,
      resolve_options, sizeof resolve_options / sizeof resolve_options[0], handle_resolve },
    { "install", "Install a toolchain", "<toolchain>", NULL, 0, handle_unimplemented },
    { "uninstall", "Remove an installed toolchain", "<toolchain>",
      NULL, 0, handle_unimplemented },
    { "default", "Set the default toolchain", "<toolchain>",
      NULL, 0, handle_unimplemented },
};

int cli_run(int argc, char **argv) {
    const cli_app app = {
        .program = "pickup",
        .version = PICKUP_VERSION,
        .tagline = "the toolchain manager for C and C++",
        .commands = commands,
        .command_count = sizeof commands / sizeof commands[0],
    };
    return cli_app_run(&app, argc, argv);
}
