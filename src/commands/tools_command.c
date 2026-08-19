#include <pickup/commands/tools_command.h>

#include <pickup/detect/tools.h>
#include <pickup/exit_code.h>
#include <pickup/util/table.h>

#include <stdio.h>

/* The columns of the human-readable table. SOURCE last, and meaning what it
   means in `list`: who is responsible for this, pickup or the system. */
static const char *const tools_headers[] = {"KIND", "NAME", "VERSION", "SOURCE"};
#define TOOLS_COLUMNS (sizeof tools_headers / sizeof tools_headers[0])

/* What is said when there is nothing to list. */
#define NOTHING_FOUND "No formatter or linter found. Try: pickup install clang-format"

static void row_of(const dev_tool *tool, const char **cells) {
    cells[0] = tool_kind_name(tool->kind);
    cells[1] = tool->name;
    cells[2] = tool->version;
    cells[3] = toolchain_source_name(tool->source);
}

static void print_text(const dev_tool *found, size_t count) {
    if (count == 0) {
        printf("%s\n", NOTHING_FOUND);
        return;
    }

    table columns;
    table_init(&columns, tools_headers, TOOLS_COLUMNS);

    /* Measured before anything is printed, so a long version string widens its
       column instead of pushing the row out of line. */
    for (size_t i = 0; i < count; i++) {
        const char *cells[TOOLS_COLUMNS];
        row_of(&found[i], cells);
        table_fit_row(&columns, cells);
    }

    table_print_header(&columns, stdout);
    for (size_t i = 0; i < count; i++) {
        const char *cells[TOOLS_COLUMNS];
        row_of(&found[i], cells);
        table_print_row(&columns, cells, stdout);
    }
}

/*
 * The machine form carries the path, which is the whole point of the command.
 *
 * A caller that means to run the formatter needs the binary, not a sentence
 * saying one exists. The table leaves it out because it would push everything
 * else off the line and a person reading it already knows where to look.
 */
static void print_toml(const dev_tool *found, size_t count) {
    for (size_t i = 0; i < count; i++) {
        printf("[[tool]]\n");
        printf("kind = \"%s\"\n", tool_kind_name(found[i].kind));
        printf("name = \"%s\"\n", found[i].name);
        printf("path = \"%s\"\n", found[i].path);
        printf("version = \"%s\"\n", found[i].version);
        printf("source = \"%s\"\n", toolchain_source_name(found[i].source));
        if (i + 1 < count)
            printf("\n");
    }
}

int tools_command_run(bool as_toml) {
    dev_tool found[TOOLS_MAX];
    size_t count = tools_discover(found, TOOLS_MAX);

    if (as_toml)
        print_toml(found, count);
    else
        print_text(found, count);
    return exit_ok;
}
