#ifndef PICKUP_TABLE_H
#define PICKUP_TABLE_H

#include <stddef.h>
#include <stdio.h>

/* Most columns a table may have. Raising it costs nothing but stack. */
#define TABLE_MAX_COLUMNS 8

/* A column layout for text output: the headers and how wide each column has to
   be to fit what goes under it.

   The table holds no rows. Callers walk their data twice, once to measure and
   once to print, because a column width is not known until the last row has
   been seen, and copying every row to avoid the second walk would cost more
   than formatting it again.

   Widths are counted in bytes, which is the same as columns for ASCII. Compiler
   names and target triples are ASCII; anything else would need a width that
   understands UTF-8. */
typedef struct {
    const char *headers[TABLE_MAX_COLUMNS];
    size_t widths[TABLE_MAX_COLUMNS];
    size_t column_count;
} table;

/* Start a table whose columns are `headers`, each initially as wide as its own
   header. `headers` must outlive the table; it is borrowed, not copied. A count
   above TABLE_MAX_COLUMNS is truncated. */
void table_init(table *columns, const char *const *headers, size_t count);

/* Widen any column too narrow for the matching cell. A NULL cell counts as
   empty. */
void table_fit_row(table *columns, const char *const *cells);

/* Current width of one column, or 0 if there is no such column. */
[[nodiscard]] size_t table_column_width(const table *columns, size_t column);

/* Write the header row. */
void table_print_header(const table *columns, FILE *out);

/* Write one row, padded to the measured widths. */
void table_print_row(const table *columns, const char *const *cells, FILE *out);

#endif /* PICKUP_TABLE_H */
