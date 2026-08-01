# Pickup Specification

Version: 0.1.0-draft

Status: Draft

---

# 1. Vision

Pickup is the toolchain manager of the Molto ecosystem.

Its purpose is to answer one question truthfully: **which compilers exist on
this machine, and what can each of them actually do?**

That answer is what lets a project manifest describe *what it needs* instead of
*which machine it was written on*. A manifest that says `compiler = "gcc-12"` is
tied to a device. A manifest that says "a C compiler with C23 attributes" is
not, and Pickup is what turns the second into the first.

Pickup provides:

- Compiler discovery
- Capability detection
- Version management
- Toolchain installation
- Global toolchain configuration

while remaining compiler agnostic.

---

# 2. Goals

Pickup should make a native toolchain as easy to reason about as a managed one.

The project prioritizes:

- Truthful discovery
- Deterministic resolution
- Zero configuration
- Fast repeated queries
- Machine-readable output
- Cross-platform support

---

# 3. Non Goals

Pickup is NOT:

- a compiler
- a build system
- a dependency manager
- a package manager for the operating system
- a replacement for GCC, Clang, or MSVC

Pickup does not build toolchains from source. Compiling GCC takes hours; Pickup
installs published binaries or reports what the system already provides.

Pickup does not build projects. That is `molto`.

Pickup does not resolve project dependencies. That is the registry.

Pickup discovers, installs, and reports. Nothing else.

---

# 4. Components

## Scanner

Finds compiler candidates on the machine.

## Probe

Interrogates a candidate to learn its identity, version, and target.

## Capability Catalog

Defines the features a compiler may support, each with a program that proves
it.

## Cache

Stores discovery results so repeated queries are fast.

## Resolver

Chooses the best toolchain that satisfies a set of requirements.

## Installer

Downloads and manages toolchains that are not present on the system.

## CLI

The `pickup` command.

---

# 5. Toolchain Model

A toolchain is a compiler Pickup can invoke.

Pickup records for each one:

- path (the real binary, symlinks resolved)
- companion C++ driver, if one exists
- vendor
- version
- target triple
- supported C features
- supported C++ features

Vendors:

- gcc
- clang
- apple-clang
- msvc
- unknown

---

# 6. Discovery

Pickup scans:

- every directory in `PATH`
- toolchains it installed itself
- extra directories the user configures

Candidates are recognized by name:

`cc`, `gcc`, `gcc-*`, `clang`, `clang-*`, `c++`, `g++`, `g++-*`, `clang++`,
`clang++-*`, `*-gcc` (cross compilers), `tcc`

A name only makes something a candidate. It never determines what it is.

Candidates are deduplicated by resolved identity, not by name. `cc`, `gcc`, and
`gcc-9` are frequently the same binary and must appear once.

---

# 7. Capability Probing

This is the core of Pickup.

**A compiler that accepts a flag has not necessarily implemented the standard
behind it.**

Measured on a single machine:

| compiler | accepts `-std=c2x` | compiles `[[nodiscard]]` |
|----------|--------------------|--------------------------|
| gcc 9    | yes                | **no**                   |
| gcc 11   | yes                | yes                      |
| gcc 12   | yes                | yes                      |

A tool that tested only the flag would report that gcc 9 supports C23. A build
system trusting that report would pick gcc 9 and fail to compile, which is the
problem Pickup exists to prevent.

Therefore:

- Pickup probes **features**, never flags.
- Every feature is proven by compiling a program that uses it.
- A probe that compiles is support. A probe that fails is not. There is no
  third answer and no version table to consult.

Pickup reports features individually, not as a monolithic "supports C23".
Standards arrive piecemeal: a compiler may implement attributes but not
`constexpr`. A project that needs one should not be denied a compiler because
of the other.

A standard is reported as supported when every feature attributed to it passes.

---

# 8. Capability Catalog

Each feature has:

- a stable identifier
- the language it belongs to
- the standard that introduced it
- a minimal program that uses it

Adding a feature means adding an entry. No other code changes.

Initial catalog:

C:

- `for_decl`, `line_comment`, `inline_fn` (C99)
- `static_assert`, `generic`, `noreturn` (C11)
- `attr_nodiscard`, `attr_maybe_unused`, `typeof`, `constexpr`, `nullptr`,
  `native_bool` (C23)

C++:

- `auto_type`, `lambda`, `nullptr_cpp` (C++11)
- `structured_bindings`, `if_constexpr`, `fold_expressions` (C++17)
- `concepts`, `spaceship` (C++20)

---

# 9. Cache

Probing is expensive: every compiler is invoked once per feature.

Pickup caches discovery results under the user's cache directory, following the
XDG convention.

A cache entry is invalidated when the binary it describes changes, detected by
path, modification time, and size.

The whole cache is discarded when the capability catalog changes. Feature sets
are stored as positions in the catalog, so entries written under a different
one name different features: reading them would report a capability that was
never proven, which is the one thing Pickup must not do. Adding, removing or
reordering an entry, or changing the program that proves a feature, all count
as a change. Rescanning costs about a second.

`--refresh` discards the cache and rescans.

A corrupt or unreadable cache is discarded, never trusted. Rescanning is always
correct; trusting bad state is not.

---

# 10. Resolution

A request states requirements:

- language
- standard, or explicit features
- optionally a preferred vendor or version

Pickup answers with one toolchain, or with an explanation.

Selection is deterministic: among the candidates that satisfy every
requirement, the highest version wins; ties are broken by vendor preference and
then by path, so the same machine always yields the same answer.

When nothing satisfies the request, Pickup reports what is missing from each
candidate. Explaining the absence is part of the answer:

```
pickup: no C compiler provides [attr_nodiscard]
  gcc-9  (9.5.0)   missing: attr_nodiscard
  clang  (14.0.0)  missing: constexpr
```

A tool that cannot say why it failed leaves the user reading compiler syntax
errors instead.

---

# 11. Installation

Pickup manages toolchains it installs, in a directory it owns, without
administrator privileges.

Layout:

```
<pickup home>/toolchains/<vendor>-<version>-<target>/
```

Installed toolchains join the same inventory as system ones. Resolution does
not distinguish between them.

Operations:

- install
- uninstall
- list installed
- set the default toolchain

Installed archives are verified before use. An artifact that fails verification
is discarded, never installed.

---

# 12. CLI

```
pickup <command> [options]
```

Commands:

- `list` — the inventory
- `show <name|path>` — one toolchain in detail, feature by feature
- `resolve` — the best toolchain for a set of requirements
- `scan` — rebuild the inventory
- `install` — add a toolchain
- `uninstall` — remove a toolchain
- `default` — set the default toolchain

Exit codes:

| Code | Meaning                                        |
|------|------------------------------------------------|
| 0    | Success                                        |
| 1    | Operation failed                               |
| 2    | Invalid usage                                  |
| 3    | No toolchain satisfies the request             |
| 4    | Command not implemented yet                    |

Exit code 3 is distinct on purpose. "Nothing matches" is an answer, not a
malfunction, and a caller must be able to tell them apart.

---

# 13. Output Formats

Every command that produces data supports:

- `text` — for people, the default
- `toml` — for machines

TOML is the machine format because Molto already reads TOML. Consuming Pickup
adds no parser to the consumer.

---

# 14. Integration with Molto

The contract between the two tools is one principle:

**The manifest declares capabilities. Pickup resolves binaries.**

Molto asks for what a project needs. Pickup answers with what this machine has.
Neither the manifest nor the project ever names a local binary.

Molto invokes Pickup as a separate process and reads its TOML output. The two
tools share no code and no memory, and either can be replaced without touching
the other.

---

# 15. Cross Platform

Supported platforms:

Linux

Windows

macOS

Supported compilers:

GCC

Clang

MSVC

Linux and GCC come first, matching Molto's roadmap.

---

# 16. Design Principles

Inherited from the Molto specification. Every feature must satisfy:

- Simple by default
- Explicit when needed
- Fast
- Portable
- Deterministic
- Backwards compatible whenever possible

And one of Pickup's own:

- **Never claim a capability that was not proven.**

---

# 17. RFC Process

All major features are specified through RFCs.

RFCs are immutable once accepted.

New behavior must be introduced through new RFCs.

---

# 18. Roadmap

Version 0.1

- Discovery
- Capability probing
- Resolution
- Linux
- GCC and Clang

Version 0.2

- Installation
- Default toolchain

Version 0.3

- Windows and macOS
- Cross-compilation targets

Version 1.0

- Stable CLI
- Stable machine-readable output
