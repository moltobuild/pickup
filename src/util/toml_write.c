#include <pickup/util/toml_write.h>

#include <stdio.h>

void toml_write_quoted(const char *value) {
    putchar('"');
    for (const char *c = value; *c != '\0'; c++) {
        switch (*c) {
        case '"':
        case '\\':
            putchar('\\');
            putchar(*c);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            /* Four escapes and no more. TOML has a fifth form for the other
               control characters, and a reader that does not know it fails on
               the whole document rather than on the byte. So a control
               character, which no path or version or vendor name has, is left
               out instead of written in a spelling that might not be read
               back. */
            if ((unsigned char)*c >= 0x20)
                putchar(*c);
            break;
        }
    }
    putchar('"');
}

void toml_write_string(const char *key, const char *value) {
    printf("%s = ", key);
    toml_write_quoted(value);
    putchar('\n');
}
