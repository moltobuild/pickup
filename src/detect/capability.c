#include <pickup/detect/capability.h>

#include <pickup/services/process_service.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Compiler arguments used to probe. */
#define ARG_STD_FORMAT "-std=%s"
#define ARG_LANG       "-x"
#define LANG_C_NAME    "c"
#define LANG_CXX_NAME  "c++"
#define ARG_STDIN      "-"      /* read the program from standard input */
#define ARG_OUTPUT     "-o"
#define OUTPUT_DISCARD "/dev/null"
#define ARG_SYNTAX_ONLY "-fsyntax-only"

/* Size of the buffer holding a composed "-std=<name>" argument. */
#define STD_FLAG_SIZE 32

/*
 * The catalog. Each program is the smallest thing that cannot compile unless
 * the feature is really there.
 *
 * The programs deliberately avoid headers: a probe should test the compiler,
 * not the standard library's completeness.
 */
static const capability catalog[] = {
    /* --- C99 --- */
    { "for_decl", lang_c, "c99",
      "int main(void){for(int i=0;i<1;i++){(void)i;}return 0;}\n" },
    { "line_comment", lang_c, "c99",
      "int main(void){// a line comment\nreturn 0;}\n" },
    { "inline_fn", lang_c, "c99",
      "static inline int f(void){return 0;}\nint main(void){return f();}\n" },

    /* --- C11 --- */
    { "static_assert", lang_c, "c11",
      "int main(void){_Static_assert(1,\"ok\");return 0;}\n" },
    { "generic", lang_c, "c11",
      "#define T(x) _Generic((x), int: 0, default: 1)\n"
      "int main(void){return T(1);}\n" },
    { "noreturn", lang_c, "c11",
      "_Noreturn static void stop(void){for(;;){}}\n"
      "int main(void){stop();}\n" },

    /* --- C23 --- */
    { "attr_nodiscard", lang_c, "c2x",
      "[[nodiscard]] static int f(void){return 0;}\nint main(void){return f();}\n" },
    { "attr_maybe_unused", lang_c, "c2x",
      "int main(void){[[maybe_unused]] int x = 0;return 0;}\n" },
    { "typeof", lang_c, "c2x",
      "int main(void){int a=0;typeof(a) b=a;return b;}\n" },
    { "constexpr", lang_c, "c2x",
      "int main(void){constexpr int n=0;return n;}\n" },
    { "nullptr", lang_c, "c2x",
      "int main(void){void *p = nullptr;return p != nullptr;}\n" },
    { "native_bool", lang_c, "c2x",
      "int main(void){bool ok = true;return ok ? 0 : 1;}\n" },

    /* --- C++11 --- */
    { "auto_type", lang_cxx, "c++11",
      "int main(){auto x = 0;return x;}\n" },
    { "lambda", lang_cxx, "c++11",
      "int main(){auto f = [](){return 0;};return f();}\n" },
    { "nullptr_cpp", lang_cxx, "c++11",
      "int main(){int *p = nullptr;return p == nullptr ? 0 : 1;}\n" },

    /* --- C++17 --- */
    { "structured_bindings", lang_cxx, "c++17",
      "struct P{int a;int b;};\n"
      "int main(){P p{0,0};auto [a,b] = p;return a+b;}\n" },
    { "if_constexpr", lang_cxx, "c++17",
      "template<class T> int f(){if constexpr(sizeof(T)>0){return 0;}else{return 1;}}\n"
      "int main(){return f<int>();}\n" },
    { "fold_expressions", lang_cxx, "c++17",
      "template<class... A> int sum(A... a){return (0 + ... + a);}\n"
      "int main(){return sum(0,0);}\n" },

    /* --- C++20 --- */
    { "concepts", lang_cxx, "c++20",
      "template<class T> concept Any = true;\n"
      "template<Any T> int f(T){return 0;}\n"
      "int main(){return f(0);}\n" },
    { "spaceship", lang_cxx, "c++20",
      "#include <compare>\n"
      "struct P{int a;auto operator<=>(const P&) const = default;};\n"
      "int main(){P x{0},y{0};return (x<=>y)==0 ? 0 : 1;}\n" },
};

#define CATALOG_COUNT (sizeof catalog / sizeof catalog[0])

_Static_assert(CATALOG_COUNT <= PICKUP_MAX_CAPABILITIES,
               "the capability bitset cannot hold the catalog");

const capability *capability_catalog(size_t *count) {
    if (count != NULL)
        *count = CATALOG_COUNT;
    return catalog;
}

size_t capability_index(const char *id) {
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (strcmp(catalog[i].id, id) == 0)
            return i;
    }
    return SIZE_MAX;
}

bool capability_set_has(capability_set set, size_t index) {
    if (index >= PICKUP_MAX_CAPABILITIES)
        return false;
    return (set.bits & (UINT64_C(1) << index)) != 0;
}

void capability_set_add(capability_set *set, size_t index) {
    if (index < PICKUP_MAX_CAPABILITIES)
        set->bits |= UINT64_C(1) << index;
}

bool capability_set_contains(capability_set available, capability_set required) {
    return (available.bits & required.bits) == required.bits;
}

capability_set capability_set_for_standard(capability_lang lang, const char *standard) {
    capability_set set = { 0 };
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (catalog[i].lang == lang && strcmp(catalog[i].standard, standard) == 0)
            capability_set_add(&set, i);
    }
    return set;
}

/* Ask `compiler` to compile `program` under `standard`. The exit status is the
   whole answer: it either accepted the feature or it did not. */
static bool proves(const char *compiler, capability_lang lang,
                   const char *standard, const char *program) {
    char std_flag[STD_FLAG_SIZE];
    snprintf(std_flag, sizeof std_flag, ARG_STD_FORMAT, standard);

    /* -fsyntax-only stops after parsing: enough to prove the feature, and it
       skips code generation and linking, which is most of the cost. */
    const char *argv[] = {
        compiler, std_flag, ARG_LANG,
        lang == lang_cxx ? LANG_CXX_NAME : LANG_C_NAME,
        ARG_SYNTAX_ONLY, ARG_STDIN, NULL
    };
    process_result result = process_try(argv, program);
    return result.completed && result.exit_code == 0;
}

bool capability_accepts_standard(const char *compiler, capability_lang lang,
                                 const char *standard) {
    /* An empty program is enough: the compiler either understands the mode or
       rejects the flag. */
    return proves(compiler, lang, standard,
                  lang == lang_cxx ? "int main(){return 0;}\n"
                                   : "int main(void){return 0;}\n");
}

capability_set capability_probe(const char *compiler, capability_lang lang) {
    capability_set set = { 0 };
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (catalog[i].lang != lang)
            continue;
        if (proves(compiler, lang, catalog[i].standard, catalog[i].program))
            capability_set_add(&set, i);
    }
    return set;
}
