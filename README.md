# Pickup

The toolchain manager of the [Molto](../molto) ecosystem. See [`spec.md`](spec.md)
and the [RFCs](rfcs/) for the design.

Pickup answers one question truthfully: **which compilers exist on this machine,
and what can each of them actually do?**

That is what lets a project manifest say *what it needs* instead of *which
machine it was written on*.

## Why probing, not version tables

A compiler that accepts a flag has not necessarily implemented the standard
behind it. On one ordinary Linux box:

| compiler | accepts `-std=c2x` | compiles `[[nodiscard]]` |
|----------|--------------------|--------------------------|
| gcc 9    | yes                | **no**                   |
| gcc 11   | yes                | yes                      |
| gcc 12   | yes                | yes                      |

Anything that tested only the flag would report gcc 9 as a C23 compiler. Pickup
proves every capability by compiling a program that uses it.

## Requirements

- `gcc-12` or newer (the build targets the C23 subset via `-std=c2x`)
- GNU Make

## Build

```sh
make build        # produces build/pickup
```

## Test

```sh
make test
```

## Project layout

```
include/pickup/  Public headers (used as <pickup/...>)
src/             CLI entry point, commands, detection, services
tests/           Test suites
modules/moltest/ Test framework, vendored from molto
```

`modules/moltest` is a copy, not a dependency: neither tool has a package
manager yet, which is precisely the problem molto exists to solve. It gets
unified through the registry once that lands.
