#include <moltest.h>

#include <pickup/util/table.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *const headers[] = { "NAME", "VENDOR", "TARGET" };
#define COLUMN_COUNT (sizeof headers / sizeof headers[0])

/* Render a row to memory so the layout can be asserted on. Returns false if
   the scratch file could not be opened, which is the caller's cue to stop:
   an assertion here would only return from this helper. */
[[nodiscard]] static bool render_row(const table *columns, const char *const *cells,
                                     char *out, size_t out_size) {
    out[0] = '\0';
    FILE *sink = tmpfile();
    if (sink == NULL)
        return false;
    table_print_row(columns, cells, sink);
    rewind(sink);
    size_t read = fread(out, 1, out_size - 1, sink);
    out[read] = '\0';
    fclose(sink);
    return true;
}

MOLTEST(table_starts_as_wide_as_its_headers) {
    table columns;
    table_init(&columns, headers, COLUMN_COUNT);

    EXPECT_EQ(4, table_column_width(&columns, 0));
    EXPECT_EQ(6, table_column_width(&columns, 1));
    EXPECT_EQ(6, table_column_width(&columns, 2));
}

MOLTEST(table_widens_a_column_to_fit_a_longer_cell) {
    table columns;
    table_init(&columns, headers, COLUMN_COUNT);

    /* The case fixed widths got wrong: a cross compiler whose name is longer
       than its header must not push the rest of the row out of line. */
    const char *const cells[] = { "x86_64-linux-gnu-gcc-12", "gcc", "-" };
    table_fit_row(&columns, cells);

    EXPECT_EQ(23, table_column_width(&columns, 0));
    EXPECT_EQ(6, table_column_width(&columns, 1));
}

MOLTEST(table_keeps_the_widest_cell_seen) {
    table columns;
    table_init(&columns, headers, COLUMN_COUNT);

    const char *const wide[] = { "gcc-12-with-a-long-name", "gcc", "-" };
    const char *const narrow[] = { "cc", "gcc", "-" };
    table_fit_row(&columns, wide);
    table_fit_row(&columns, narrow);

    /* A later, shorter row never shrinks a column back. */
    EXPECT_EQ(23, table_column_width(&columns, 0));
}

MOLTEST(table_pads_every_column_but_the_last) {
    table columns;
    table_init(&columns, headers, COLUMN_COUNT);

    const char *const cells[] = { "cc", "gcc", "x86_64-linux-gnu" };
    table_fit_row(&columns, cells);

    char row[128];
    ASSERT_TRUE(render_row(&columns, cells, row, sizeof row));

    /* Padded to the header widths, two spaces between columns, and no blanks
       left dangling at the end of the line. */
    EXPECT_STREQ("cc    gcc     x86_64-linux-gnu\n", row);
}

MOLTEST(table_treats_a_missing_cell_as_empty) {
    table columns;
    table_init(&columns, headers, COLUMN_COUNT);

    const char *const cells[] = { "cc", NULL, NULL };
    table_fit_row(&columns, cells);

    char row[128];
    ASSERT_TRUE(render_row(&columns, cells, row, sizeof row));

    EXPECT_EQ(6, table_column_width(&columns, 1));
    EXPECT_STREQ("cc            \n", row);
}

MOLTEST(table_truncates_more_columns_than_it_can_hold) {
    const char *const many[TABLE_MAX_COLUMNS + 3] = { 0 };
    table columns;
    table_init(&columns, many, TABLE_MAX_COLUMNS + 3);

    EXPECT_EQ(TABLE_MAX_COLUMNS, columns.column_count);
}
