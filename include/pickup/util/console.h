#ifndef PICKUP_CONSOLE_H
#define PICKUP_CONSOLE_H

#include <stdbool.h>

/*
 * The terminal Pickup draws on, prepared once before anything is drawn.
 *
 * On every Unix this is nothing: a terminal honours escape sequences and reads
 * UTF-8 because that is what a terminal does. On Windows neither is true by
 * default, and both are needed by the same two lines of output -- the download
 * bar and the extraction spinner -- which is why they are settled together
 * here rather than by whoever happens to print first.
 *
 *   Sequences. A console starts with ENABLE_VIRTUAL_TERMINAL_PROCESSING off,
 *   so `\x1b[36m` is printed as the four characters it is made of. It is a
 *   mode on the handle, asked for once; a console too old to grant it says so,
 *   and then colour has to stay off rather than be written and not understood.
 *
 *   Code page. The bar is drawn with U+2588 and U+2591, which reach the
 *   console as UTF-8. A console on code page 850 reads those bytes as three
 *   letters each and draws mojibake. The page is a property of the console
 *   rather than of this process, so the one that was there is put back on the
 *   way out.
 */

/* Prepare the console, once, before anything is written to it. */
void console_configure(void);

/* Whether an escape sequence written to the console will be acted on rather
   than printed. Answered by console_configure; false until it has run. */
[[nodiscard]] bool console_supports_sequences(void);

#endif /* PICKUP_CONSOLE_H */
