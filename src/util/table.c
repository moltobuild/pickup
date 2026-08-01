#include <pickup/util/table.h>

#include <string.h>

/* Blank between one column and the next. Two spaces separate the columns
   without a rule, which keeps the output something `cut` and `awk` can still
   read. */
#define COLUMN_GAP "  "

static size_t cell_width(const char *cell) {
    return cell != NULL ? strlen(cell) : 0;
}

void table_init(table *columns, const char *const *headers, size_t count) {
    columns->column_count = count < TABLE_MAX_COLUMNS ? count : TABLE_MAX_COLUMNS;
    for (size_t i = 0; i < columns->column_count; i++) {
        columns->headers[i] = headers[i];
        columns->widths[i] = cell_width(headers[i]);
    }
}

void table_fit_row(table *columns, const char *const *cells) {
    for (size_t i = 0; i < columns->column_count; i++) {
        size_t width = cell_width(cells[i]);
        if (width > columns->widths[i])
            columns->widths[i] = width;
    }
}

size_t table_column_width(const table *columns, size_t column) {
    return column < columns->column_count ? columns->widths[column] : 0;
}

/* The last column is never padded: trailing blanks are invisible on screen and
   only get in the way of whoever pipes the output somewhere else. */
static void print_cells(const table *columns, const char *const *cells, FILE *out) {
    for (size_t i = 0; i < columns->column_count; i++) {
        const char *cell = cells[i] != NULL ? cells[i] : "";
        if (i + 1 == columns->column_count)
            fprintf(out, "%s\n", cell);
        else
            fprintf(out, "%-*s" COLUMN_GAP, (int)columns->widths[i], cell);
    }
}

void table_print_header(const table *columns, FILE *out) {
    print_cells(columns, columns->headers, out);
}

void table_print_row(const table *columns, const char *const *cells, FILE *out) {
    print_cells(columns, cells, out);
}
