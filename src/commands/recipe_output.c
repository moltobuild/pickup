#include <pickup/commands/recipe_output.h>

#include <pickup/util/toml_write.h>

#include <stdio.h>

/* Print a TOML array of strings on one line. */
static void print_string_array(const char *key, const char rows[][RECIPE_FLAG_MAX], size_t count) {
    printf("%s = [", key);
    for (size_t i = 0; i < count; i++) {
        fputs(i == 0 ? "" : ", ", stdout);
        toml_write_quoted(rows[i]);
    }
    printf("]\n");
}

static void print_dir_array(const char *key, const char rows[][PICKUP_PATHS_MAX], size_t count) {
    printf("%s = [", key);
    for (size_t i = 0; i < count; i++) {
        fputs(i == 0 ? "" : ", ", stdout);
        toml_write_quoted(rows[i]);
    }
    printf("]\n");
}

void recipe_print_toml(capability_lang lang, const link_recipe *recipe) {
    printf("\n[%s]\n", lang == lang_cxx ? "cxx" : "c");

    /* Which standard library the flags commit the build to. Published for C++
       because it is an ABI, not a preference: objects built against libc++ and
       against libstdc++ cannot be linked together, so a caller pulling in a
       prebuilt library has to know which one it is looking at. */
    if (lang == lang_cxx)
        toml_write_string("stdlib", recipe_stdlib_name(recipe->stdlib));

    print_string_array("compile_flags", recipe->compile_flags, recipe->compile_count);
    print_string_array("link_flags", recipe->link_flags, recipe->link_count);
    /* Where the shared libraries the produced program needs actually live.
       Linking is not running, and a caller that has to launch what it built,
       or ship it, cannot derive these from the flags. */
    print_dir_array("runtime_dirs", recipe->runtime_dirs, recipe->runtime_count);
}
