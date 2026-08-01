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

## Usage

```sh
pickup list                # the inventory
pickup show <name>         # one toolchain, feature by feature
pickup scan                # re-probe everything and rewrite the cache
pickup resolve [options]   # the best toolchain for a set of requirements
```

`install`, `uninstall` and `default` are specified but not implemented yet.

### Looking at what the machine has

`list` prints the inventory, deduplicated by resolved identity and ordered by
version, so `cc`, `gcc` and `gcc-9` appear once:

```
$ pickup list
NAME     VENDOR  VERSION  TARGET
clang    clang   14.0.0   x86_64-pc-linux-gnu
gcc-12   gcc     12.3.0   x86_64-linux-gnu
cc       gcc     9.5.0    x86_64-linux-gnu
```

Columns are measured against what is in them, so a name as long as
`x86_64-linux-gnu-gcc-12` widens its column instead of pushing the row out of
line. Nothing is padded past the last column, which keeps the output readable
by `awk` and `cut`.

`show` is where the probing becomes visible. Both of these accept `-std=c2x`;
only one of them implements it:

```
$ pickup show cc
cc
  path     /usr/bin/cc
  vendor   gcc
  version  9.5.0
  target   x86_64-linux-gnu
  c++      /usr/bin/c++

C features
  ...
  c2x
    attr_nodiscard           no
    attr_maybe_unused        no
    typeof                   no
    constexpr                no
    nullptr                  no
    native_bool              no
```

(C++ features and the passing C99 and C11 groups are elided above.)

The first query probes every compiler once and caches the result; later ones
answer from the cache in milliseconds. Run `scan` after installing or upgrading
a compiler to rebuild it.

### Asking for a toolchain

`resolve` takes requirements and answers with one compiler, or explains the
absence:

```sh
pickup resolve --require attr_nodiscard        # proven features
pickup resolve --lang c++ --std c++20          # language and standard flag
pickup resolve --vendor gcc --format toml      # for machines
```

**`--require` and `--std` are not the same question, and the difference
matters.** `--require` names features and is answered by probing: a compiler
passes only if a program using each one compiled. `--std` is answered by the
flag alone — it asks whether the driver can be invoked in that mode, which is
what the caller needs in order to build the compile line.

So `--std` on its own is not a guarantee about the implementation. On the
machine these examples came from, gcc 12 accepts `-std=c2x` while failing four
of the features attributed to C23, and gcc 9 accepts it while failing six. Ask
for both when the code depends on both:

```sh
pickup resolve --std c2x --require attr_nodiscard
```

When nothing matches, Pickup names what each candidate lacks rather than
leaving the user to read compiler errors later:

```
$ pickup resolve --require constexpr,nullptr
pickup: no c compiler satisfies the request
  clang            (14.0.0) missing: constexpr nullptr
  gcc-12           (12.3.0) missing: constexpr nullptr
```

### Output for machines

Every command that produces data takes `--format toml`. TOML because Molto
already parses it: consuming Pickup adds no parser to the consumer.

```
$ pickup resolve --std c2x --require attr_nodiscard --format toml
[compiler]
path = "/usr/bin/clang"
c_path = "/usr/bin/clang"
cxx_path = "/usr/bin/clang++"
vendor = "clang"
version = "14.0.0"
target = "x86_64-pc-linux-gnu"
std_flag = "-std=c2x"
```

### Exit codes

| Code | Meaning                            |
|------|------------------------------------|
| 0    | Success                            |
| 1    | Operation failed                   |
| 2    | Invalid usage                      |
| 3    | No toolchain satisfies the request |
| 4    | Command not implemented yet        |

Code 3 is distinct on purpose. "Nothing matches" is an answer, not a
malfunction, and a caller must be able to tell them apart.

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
