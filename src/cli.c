#include <pickup/cli.h>

#include <pickup/commands/doctor_command.h>
#include <pickup/commands/install_command.h>
#include <pickup/commands/list_command.h>
#include <pickup/commands/resolve_command.h>
#include <pickup/commands/scan_command.h>
#include <pickup/commands/search_command.h>
#include <pickup/commands/show_command.h>
#include <pickup/exit_code.h>
#include <pickup/util/cli.h>
#include <pickup/util/color.h>

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
    { "--stdlib", 0, cli_opt_value, "<libstdc++|libc++>",
      "C++ standard library the build must use", NULL },
    { "--format", 'f', cli_opt_value, "<text|toml>", "Output format", FORMAT_TEXT },
};

/* What `pickup search` accepts. */
static const cli_option search_options[] = {
    { "--version", 'v', cli_opt_value, "<version>",
      "Only versions matching this, whole or partial", NULL },
    { "--refresh", 0, cli_opt_flag, NULL,
      "Ask the source again instead of using the cached index", NULL },
    { "--format", 'f', cli_opt_value, "<text|toml>", "Output format", FORMAT_TEXT },
};

/* What `pickup install` accepts. */
static const cli_option install_options[] = {
    { "--version", 'v', cli_opt_value, "<version>",
      "Version to install, whole or partial (default: the latest stable)", NULL },
    { "--allow-unverified", 0, cli_opt_flag, NULL,
      "Install even if the release publishes no sha256", NULL },
    { "--dry-run", 0, cli_opt_flag, NULL,
      "Report what would be installed and download nothing", NULL },
    { "--refresh", 0, cli_opt_flag, NULL,
      "Ask the source again instead of using the cached index", NULL },
    { "--full", 0, cli_opt_flag, NULL,
      "Unpack the entire release instead of only what is needed to build", NULL },
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
        .stdlib = cli_args_option(args, "--stdlib"),
    };
    return resolve_command_run(&request, wants_toml(args));
}

static int handle_scan(const cli_args *args) {
    (void)args;
    return scan_command_run();
}

static int handle_search(const cli_args *args) {
    const search_command_request request = {
        .name = cli_args_positional(args, 0),
        .version = cli_args_option(args, "--version"),
        .refresh = cli_args_flag(args, "--refresh"),
        .as_toml = wants_toml(args),
    };
    return search_command_run(&request);
}

static int handle_install(const cli_args *args) {
    const install_command_request request = {
        .name = cli_args_positional(args, 0),
        .version = cli_args_option(args, "--version"),
        .allow_unverified = cli_args_flag(args, "--allow-unverified"),
        .dry_run = cli_args_flag(args, "--dry-run"),
        .refresh = cli_args_flag(args, "--refresh"),
        .full = cli_args_flag(args, "--full"),
    };
    return install_command_run(&request);
}

static int handle_doctor(const cli_args *args) {
    return doctor_command_run(wants_toml(args));
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
    { "doctor", "Report what stops this machine from building, and what fixes it",
      NULL, format_option, sizeof format_option / sizeof format_option[0],
      handle_doctor },
    { "resolve", "Find the best toolchain for a set of requirements", NULL,
      resolve_options, sizeof resolve_options / sizeof resolve_options[0], handle_resolve },
    { "search", "List the versions of a toolchain available to install", "<clang|gcc>",
      search_options, sizeof search_options / sizeof search_options[0], handle_search },
    { "install", "Download and install a toolchain", "<clang|gcc>",
      install_options, sizeof install_options / sizeof install_options[0],
      handle_install },
    { "uninstall", "Remove an installed toolchain", "<toolchain>",
      NULL, 0, handle_unimplemented },
    { "default", "Set the default toolchain", "<toolchain>",
      NULL, 0, handle_unimplemented },
};

int cli_run(int argc, char **argv) {
    /* Decided once, from the stream the reports go to. */
    color_configure(stdout);

    const cli_app app = {
        .program = "pickup",
        .version = PICKUP_VERSION,
        .tagline = "the toolchain manager for C and C++",
        .commands = commands,
        .command_count = sizeof commands / sizeof commands[0],
    };
    return cli_app_run(&app, argc, argv);
}
