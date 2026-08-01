#include <pickup/commands/scan_command.h>

#include <pickup/commands/probe_progress.h>
#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>

#include <stdio.h>

int scan_command_run(void) {
    inventory list;
    /* Always probes, by definition: this is the command that exists to throw
       the cache away, so it is the one that always has something to report. */
    if (!probe_progress_load(&list, true)) {
        fprintf(stderr, "pickup: could not scan for compilers\n");
        return exit_failure;
    }
    printf("Probed %zu toolchain%s\n", list.count, list.count == 1 ? "" : "s");
    inventory_free(&list);
    return exit_ok;
}
