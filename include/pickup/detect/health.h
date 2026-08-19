#ifndef PICKUP_HEALTH_H
#define PICKUP_HEALTH_H

#include <stdbool.h>
#include <stddef.h>

#include <pickup/detect/capability.h>
#include <pickup/model/toolchain.h>

/*
 * Whether a compiler can actually produce a working program.
 *
 * The capability catalog deliberately avoids headers, because it is testing the
 * language rather than the library that ships beside it (capability.c). That is
 * the right call, and it leaves an entire axis unmeasured: a compiler can
 * implement every feature of C++20 and still fail on `#include <iostream>`,
 * because the standard library it borrows is incomplete.
 *
 * So this asks the other question, in the only order that tells the answers
 * apart:
 *
 *   does the header resolve   -> otherwise the standard library is missing
 *   does it link              -> otherwise the runtime is missing
 *   does the binary run       -> otherwise its libraries are somewhere the
 *                                loader will not look
 *
 * The last step is the one that cannot be skipped. A link that exits zero and
 * an executable that dies with "cannot open shared object file" is exactly the
 * kind of claim Pickup exists to refuse: everything measured said yes, and
 * nothing works.
 */

typedef enum {
    health_ok,
    health_no_driver,  /* there is no driver for that language at all */
    health_no_headers, /* the standard header does not resolve */
    health_no_link,    /* it compiles and will not link */
    health_no_run,     /* it links and the executable will not start */
} health_status;

typedef struct {
    health_status c;
    health_status cxx;
} toolchain_health;

/* A one-line explanation, for a caller reporting to a person. Never NULL. */
[[nodiscard]] const char *health_status_message(health_status status);

/* Compile, link and run a standard-library program with `compiler`, adding
   `flags` to every invocation, and report how far it got.

   The flags are what makes this reusable: recipe_discover proves a candidate
   set of flags by running exactly this, so the recipe it publishes is one that
   was watched producing a program that ran. */
[[nodiscard]] health_status health_probe(const char *compiler, capability_lang lang,
                                         const char *const *flags, size_t flag_count);

/* The same for both languages of a toolchain, with no flags: what the compiler
   does when invoked the way a person would invoke it. */
[[nodiscard]] toolchain_health health_check(const toolchain *chain);

/* True if `status` means the language can be built with. */
[[nodiscard]] bool health_is_usable(health_status status);

#endif /* PICKUP_HEALTH_H */
