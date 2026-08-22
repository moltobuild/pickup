#include <pickup/commands/host_command.h>

#include <pickup/exit_code.h>
#include <pickup/sources/registry_source.h>

#include <stdio.h>

int host_command_run(bool as_toml) {
    const char *target = registry_host_target();

    /* An empty target is a platform nothing is published for. Printing it as a
       blank value would look like an answer; saying so and exiting 3 lets a
       caller tell "this host has no artifacts" from "pickup broke". */
    if (target[0] == '\0') {
        fprintf(stderr, "pickup: the registry publishes nothing that could run on this host\n");
        return exit_no_match;
    }

    if (as_toml)
        printf("target = \"%s\"\n", target);
    else
        printf("%s\n", target);
    return exit_ok;
}
