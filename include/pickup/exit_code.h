#ifndef PICKUP_EXIT_CODE_H
#define PICKUP_EXIT_CODE_H

/* Process exit codes as specified in spec.md section 12. */
typedef enum {
    exit_ok = 0,
    exit_failure = 1,
    exit_usage_error = 2,
    /* No toolchain satisfies the request. Distinct from a failure on purpose:
       "nothing matches" is an answer, and a caller must be able to tell the
       two apart. */
    exit_no_match = 3,
    exit_not_implemented = 4,
} pickup_exit_code;

#endif /* PICKUP_EXIT_CODE_H */
