#include <pickup/commands/scan_command.h>

#include <pickup/exit_code.h>
#include <pickup/services/inventory_service.h>

#include <stdio.h>

int scan_command_run(void) {
    inventory list;
    if (!inventory_load(&list, true)) {
        fprintf(stderr, "pickup: could not scan for compilers\n");
        return exit_failure;
    }
    printf("Probed %zu toolchain%s\n", list.count, list.count == 1 ? "" : "s");
    inventory_free(&list);
    return exit_ok;
}
