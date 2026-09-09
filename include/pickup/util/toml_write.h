#ifndef PICKUP_TOML_WRITE_H
#define PICKUP_TOML_WRITE_H

/*
 * Writing TOML that a TOML reader accepts.
 *
 * Every machine-readable answer Pickup gives is TOML, and most of the strings
 * in one are paths. On Windows a path is full of backslashes, and a backslash
 * inside a TOML basic string opens an escape: `"C:\Users\black"` is not a
 * string with two backslashes in it, it is a parse error at `\U`. Printed with
 * a bare `%s` the whole document is unreadable — which is how `molto run` on
 * Windows came to fail before a single file was compiled.
 *
 * So nothing prints a TOML string with printf directly. It goes through here,
 * where the four characters that need saying differently are said.
 */

/* Print `key = "value"`, followed by a newline, with `value` escaped. */
void toml_write_string(const char *key, const char *value);

/* Print just the quoted, escaped `value` — for the members of an array, where
   the caller owns the brackets and the separators. */
void toml_write_quoted(const char *value);

#endif /* PICKUP_TOML_WRITE_H */
