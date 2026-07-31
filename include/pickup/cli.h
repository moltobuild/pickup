#ifndef PICKUP_CLI_H
#define PICKUP_CLI_H

/* Parse `argv` and dispatch to a command. Returns a pickup_exit_code. */
[[nodiscard]] int cli_run(int argc, char **argv);

/* The version string reported by --version. */
[[nodiscard]] const char *cli_version(void);

#endif /* PICKUP_CLI_H */
