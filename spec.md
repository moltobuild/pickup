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

## Health and Recipe

What the catalog does not measure: whether a compiler produces a program that
runs, and which flags make it do so.

## Diagnostics

Why this machine cannot build, reported by cause rather than by symptom.

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

# 7a. Health and the Build Recipe

Feature probes deliberately avoid headers: they test the language, not the
completeness of the library shipped beside it. That is the right boundary, and
it leaves a second question unanswered — **can this compiler produce a program
that runs?**

A compiler can implement every feature of C++20 and fail on `#include
<iostream>`, because on Linux it borrows its C++ standard library, its startup
objects and libgcc from a GCC installation it locates at run time. If that
installation is missing its C++ half, C compiles perfectly and C++ does not.

So health is measured in the order that tells the causes apart:

- the header resolves, or the standard library is missing
- it links, or the runtime is missing
- **the binary runs**, or its libraries are somewhere the loader will not look

The last is not optional. A link that exits zero and an executable that dies on
`cannot open shared object file` is a toolchain that passed every measurement
and builds nothing — the one claim Pickup must never make.

A toolchain is therefore a binary *plus the flags that make it work*, and those
flags are the answer as much as the path is. They are discovered, not assumed:
candidates are tried in order and the first that compiles, links and runs is
published.

libstdc++ is preferred over libc++ where both work. The two are not ABI
compatible, and libstdc++ is what the rest of the machine was built against;
the compiler's own library makes a toolchain self-contained at the cost of no
longer linking against the system's. That is a real cost, so it is a last
resort rather than a default.

---

# 7b. Diagnostics

**A finding is a cause, not a symptom.**

One GCC installed without its C++ half breaks every Clang that selects it and
leaves a `g++` that is not there. Reported separately, those read as unrelated
faults and send a reader chasing several problems where there is one; reported
together, they name the single thing to fix and explain what was never obvious
— why a package missing from GCC breaks Clang.

**Severity is impact, not imperfection.** A thing can be broken and prevent
nothing: a GCC without its C++ half, on a machine with other toolchains that
build C and C++, stops nobody. Failing over it would make the exit code useless
for what an exit code is for, so what counts is whether anything is left
undoable. What that leaves out is still true, and still shown on request.

A report that only lists problems answers *what is broken* and leaves *what
does this machine have* unanswered, which is asked far more often. The state of
things is stated whether or not anything is wrong.

Beyond compilers, a machine is reported on for the tools a project is worked on
with — a formatter and a linter. Found the way compilers are, by name and then
by asking: one that does not answer is a file with a name, not a tool.

Diagnosing that one is missing and saying where the others are are different
questions, and are answered by different commands. A caller that means to run a
formatter needs its path, and a report of problems is the wrong place to look
for one.

One thing is done rather than reported, and it stays on Pickup's own side of
the line. A toolchain's configuration file records a decision made when it was
installed and nothing revalidates it, so it can go on naming a compiler that
has since stopped being the right one. Since the recipes have just been worked
out afresh, bringing the file into step costs nothing — and only ever for
toolchains Pickup installed, only for files Pickup wrote, and never in silence.

Remedies are named, never applied. Repairing the system is not what a diagnosis
is for, and the useful remedies are rarely equivalent: one repairs what is
broken, another sidesteps it and leaves it broken. Which to take is the
reader's decision, so each says what it does.

Pickup names a package of the operating system without becoming its package
manager. The distribution is read from `/etc/os-release`; only the package name
comes from convention, and where the distribution is not recognised the
suggestion is omitted rather than invented.

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

Pickup caches discovery results under the pickup home, alongside everything else
it writes:

```
<pickup home>/cache/       regenerable: the probed inventory, the release index
<pickup home>/downloads/   archives in flight, removed once installed
<pickup home>/toolchains/  what was installed, one directory each
```

The first two can be deleted at any time and cost time, never correctness. The
third is what the downloads were for. One root rather than two means one
variable, `PICKUP_HOME`, relocates the lot — which is also how the tests run
without touching a real home directory.

A cache entry is invalidated when the binary it describes changes, detected by
path, modification time, and size.

The whole cache is discarded when the capability catalog changes. Feature sets
are stored as positions in the catalog, so entries written under a different
one name different features: reading them would report a capability that was
never proven, which is the one thing Pickup must not do. Adding, removing or
reordering an entry, or changing the program that proves a feature, all count
as a change. Rescanning costs about a second.

`scan` discards the inventory and probes everything again. The index of
published releases is a separate cache with its own short lifetime, discarded by
`--refresh` on `search` and `install`: the two answer different questions and
go stale for different reasons.

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

The pickup home is `~/.pickup`, or `$PICKUP_HOME` where that is set. Nothing is
ever written outside it, so installing never requires elevation and uninstalling
is deleting a directory.

The version and target in that name are read from the installed compiler, not
from the name of the archive it came in. Unpacking something and asking it what
it is proves it runs; trusting a file name proves nothing.

Installed toolchains join the same inventory as system ones. Resolution does
not distinguish between them.

Operations:

- search
- install
- uninstall
- list installed
- set the default toolchain

## Sources

A source knows what a vendor publishes and which of it runs here. The first is
LLVM, read from GitHub's REST API: releases marked as prereleases or drafts are
never offered, and one release ships source archives, documentation, installers
and several platforms at once, so the asset for the host is selected rather than
assumed. Asset naming has changed across LLVM versions, so it is matched, never
constructed.

The GNU project publishes no binaries of its own, so a GCC comes from
conda-forge, read straight from the channel over HTTPS. No client is installed
and no environment is activated: the channel answers JSON, which Pickup already
parses, and ships packages as zip files holding tarballs, which it can already
open.

A compiler there is a closure of packages rather than one archive. Gathering it
is a walk, not a search: take the newest build satisfying each requirement,
follow what it names, stop when nothing new appears. There is no backtracking,
because the constraints of one toolchain are already consistent — the channel
built those packages against each other — and a toolchain whose constraints
needed searching is one Pickup should refuse rather than guess at. A
requirement that cannot be met stops the walk and is named, and nothing is
downloaded.

This does not make Pickup a package manager. It resolves the closure of a
toolchain and nothing else.

Packages record the path they were built under, so that path is rewritten to
the real one on the way in. Binaries keep their length, since an ELF is full of
offsets into itself, and the difference is made up with NUL bytes at the end of
the string rather than immediately after the prefix — a linker embeds its own
script as text, and padding in the middle of it truncates a path and leaves a
script that will not parse.

## What gets installed

A release carries far more than a compiler. LLVM 22.1.8 unpacks to 11.29 GB, of
which the compiler is 259 MB; the rest is MLIR, Flang, lldb, the refactoring
tools, and gigabytes of static libraries for linking against LLVM itself.

Pickup installs what the ecosystem uses — the compiler, a linker, the builtin
headers, the runtime and libc++ — which comes to about 550 MB. Nothing else is
ever written: the archive streams past and only matching entries are unpacked,
so the full contents never need to fit on the disk at all.

Whatever is unpacked is asked to compile, link and **run** a program before it
is adopted. Counting proven features is not enough on its own: those probes
avoid headers on purpose, so a compiler whose sysroot never arrived, or whose
embedded paths were rewritten wrongly, passes every one of them and then fails
on the first real translation unit.

The selection is a claim about someone else's layout, and layouts change. So it
is not trusted: once unpacked, the compiler is asked to compile, and a toolchain
that proves nothing means the selection was wrong. Then the release is unpacked
whole rather than leaving something installed that cannot build. `--full` skips
the selection from the start.

Probing the result is the same test Pickup applies to every compiler it reports
on, turned on its own installer.

## Verification

Installed archives are verified before use. An artifact that fails verification
is discarded, never installed, and never unpacked.

The digest comes from the source alongside the download. Where a release
publishes none — LLVM only began doing so recently — Pickup refuses to install
it rather than pretend it checked, and says what flag will override that. The
decision is made before downloading: spending a gigabyte to arrive at the same
refusal helps nobody.

An install is assembled under a temporary name and moved into place only once it
is complete, which is atomic within a filesystem. An interrupted install
therefore leaves nothing that looks finished.

---

# 12. CLI

```
pickup <command> [options]
```

Commands:

- `list` — the inventory
- `show <name|path>` — one toolchain in detail, feature by feature
- `tools` — the formatter and linter this machine has, and where they are
- `doctor` — what stops this machine from building, and what would fix it
- `resolve` — the best toolchain for a set of requirements
- `scan` — rebuild the inventory
- `search <toolchain>` — the versions available to install
- `install <toolchain>` — add a toolchain
- `uninstall` — remove a toolchain
- `default` — set the default toolchain

`search` and `install` take `--version`, which may name fewer components than
the version has: `14` means any 14, `14.2` any 14.2. Without it, `install`
takes the newest stable release.

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
