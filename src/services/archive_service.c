#include <pickup/services/archive_service.h>

#include <pickup/services/process_service.h>

#include <stdio.h>

/* The extractor Pickup drives.
   -x extract, -f from a file, -C into a directory */
#define TAR_COMMAND    "tar"
#define ARG_EXTRACT    "-xf"
#define ARG_DIRECTORY  "-C"
#define ARG_STRIP_FORMAT "--strip-components=%d"
#define ARG_VERSION    "--version"

/* Room for the composed --strip-components argument. */
#define STRIP_ARG_SIZE 32

const char *archive_requirement(void) {
    return TAR_COMMAND;
}

bool archive_available(void) {
    static bool checked = false;
    static bool available = false;
    if (checked)
        return available;

    const char *argv[] = { TAR_COMMAND, ARG_VERSION, NULL };
    process_result result = process_try(argv, NULL);
    available = result.completed && result.exit_code == 0;
    checked = true;
    return available;
}

bool archive_extract(const char *archive, const char *destination, int strip_components) {
    if (!archive_available())
        return false;

    char strip[STRIP_ARG_SIZE];
    snprintf(strip, sizeof strip, ARG_STRIP_FORMAT, strip_components);

    const char *argv[] = {
        TAR_COMMAND, ARG_EXTRACT, archive, ARG_DIRECTORY, destination, strip, NULL
    };
    process_result result = process_try(argv, NULL);
    return result.completed && result.exit_code == 0;
}
