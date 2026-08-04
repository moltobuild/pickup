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
pickup doctor              # what stops this machine from building, and what fixes it
pickup scan                # re-probe everything and rewrite the cache
pickup resolve [options]   # the best toolchain for a set of requirements
pickup search <toolchain>  # the versions available to install
pickup install <toolchain> # download and install one
```

`uninstall` and `default` are specified but not implemented yet.

### Looking at what the machine has

`list` prints the inventory, one row per compiler:

```
$ pickup list
NAME              VENDOR  VERSION  TARGET                    SOURCE
clang@22.1.8      clang   22.1.8   x86_64-unknown-linux-gnu  pickup
clang@14.0.0      clang   14.0.0   x86_64-pc-linux-gnu       system
gcc@12.3.0-conda  gcc     12.3.0   x86_64-conda-linux-gnu    pickup
gcc@12.3.0        gcc     12.3.0   x86_64-linux-gnu          system
gcc@11.4.0        gcc     11.4.0   x86_64-linux-gnu          system
gcc@9.5.0         gcc     9.5.0    x86_64-linux-gnu          system
```

**A toolchain is named for what it is, not for the link that led to it.** One
GCC answers to `cc`, `gcc-9`, `c89-gcc` and `g++-9`; four rows of that say four
things where there is one, and none of the four names tells you which compiler
it is. The identity is the vendor and the version, in the shape package
managers already use.

Where two toolchains would share a name, the target settles it: a GCC built for
`x86_64-conda-linux-gnu` becomes `gcc@12.3.0-conda` and does not collide with
the system's own 12.3.0. The suffix comes from the target itself and never from
comparing toolchains with each other — a name that changed because something
unrelated was installed would be no name at all.

`SOURCE` says who is responsible for it: `pickup` for one Pickup installed and
can remove again, `system` for one that was already there and belongs to the
distribution's package manager.

Collapsing the aliases keeps everything measured. Each spelling is probed on
its own, and a bare `c++` has no C++ driver to derive from its name, so its C++
features come back empty while the `cc` beside it finds one; taking either
alone would report less than was proven.

Any shorter form of the name works wherever one is asked for:

```sh
pickup show gcc@12.3.0-conda   # exactly that one
pickup show gcc@12.3.0         # the version, whichever target
pickup show gcc@12             # the newest 12
pickup show gcc@latest         # or just: pickup show gcc
```

When a query genuinely names more than one — two toolchains at the same version
— the chosen one is still shown, and stderr says what else it could have meant.

Columns are measured against what is in them, so a name as long as
`x86_64-linux-gnu-gcc-12` widens its column instead of pushing the row out of
line. Nothing is padded past the last column, which keeps the output readable
by `awk` and `cut`.

The first `list` on a machine has to probe, and probing is seventeen compilers
being made to compile. That is long enough to look like a hang, so it says how
far it has got:

```
$ pickup list
probing compilers  [██████████████░░░░░░░░░░░]   58%  10/17
```

The line is wiped before the table is printed, so what remains is the answer and
nothing else. `resolve` shows the same bar, and `scan` always does: throwing the
cache away is what it is for, so it is the one command that never has a cached
answer to fall back on. A `list` or a `resolve` served from the cache probes
nothing and draws nothing.

`search` reports the one part of it that touches the network. A spinner rather
than a bar, because the API never says how long its answer will be:

```
$ pickup search clang
fetching the release index  \
```

All of it goes to stderr, never to stdout:
`pickup list --format toml > toolchains.toml` gets a file with a progress bar in
it from nobody, and `resolve` piped into a build stays exactly as parseable as
it was.

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

### What is wrong with this machine

The capability catalog tests the *language*, on purpose: its probes avoid
headers so that a compiler is not failed for the state of the library beside
it. That leaves a second question, which `doctor` asks — can this compiler
produce a program that runs?

```
$ pickup doctor
compilers
  ✓ 6 found - clang 14.0.0 to 22.1.8, gcc 9.5.0 to 12.3.0
  ✓ 4 of them build C and C++

tools
  ✗ formatter none found
      -> pickup install clang-format
  ✗ linter    none found
      -> pickup install clang-tidy

1 more thing worth knowing; pickup doctor --all
```

**`✗` is what leaves you unable to do something.** A GCC installed without its
C++ half is broken and will stay broken, but on a machine with four toolchains
that build C and C++ it prevents nothing — and a command that failed over it
would be one no script could act on. The exit code follows the same rule.

A report that only lists problems answers *what is broken* and leaves *what
does this machine have* unanswered, which is the question asked far more often.
So the summary lines come first, and appear whether or not anything is wrong.

Nothing is hidden silently: the count at the end says how much was left out,
and `--all` shows it with its remedies.

```
$ pickup doctor --all
compilers
  ✓ 6 found - clang 14.0.0 to 22.1.8, gcc 9.5.0 to 12.3.0
  ✓ 4 of them build C and C++
  ✓ gcc 12  /usr/lib/gcc/x86_64-linux-gnu/12
      installed without C++ support: no libstdc++ headers
        clang@14.0.0 selects this GCC and cannot find <iostream>
        gcc@12.3.0 has no C++ driver, so it compiles C only
      -> install the g++-12 package - completes this GCC, and fixes everything above
      -> pickup install gcc --version 12 - a separate toolchain, no root; leaves the one above untouched
```

**A finding is a cause, not a symptom.** One GCC installed without its C++ half
shows up in two unrelated-looking places, and reporting them separately would
send a reader chasing two problems where there is one — and hide the only part
that was not obvious: why a package missing from GCC breaks Clang.

The two remedies are not interchangeable, so each says what it does. Only the
first repairs the installation, and with it every compiler standing on it. The
second installs a toolchain of Pickup's own without administrator rights and
leaves the broken one exactly where it was.

Remedies are named, never applied.

### The tools a project is worked on with

A machine that compiles is not a machine that can be worked on. `doctor` looks
for a formatter and a linter — `clang-format`, `clang-tidy`, `cppcheck` — on
PATH and in what Pickup installed, and asks each one to identify itself: a file
with the right name that does not answer is a file, not a tool.

```sh
pickup install clang-format
pickup install clang-tidy
```

Both come from conda-forge, through the same closure resolution as a compiler.
What differs is the test they have to pass before being adopted: there is
nothing here to compile, so it is that the installed binary runs and answers.
They land under `~/.pickup/tools/`, apart from the toolchains, because nothing
resolves against them.

A toolchain of LLVM carries both already, so `pickup install clang` keeps them
rather than pruning them away — about 100 MB more, almost all of it
`clang-tidy`, out of an archive that is downloaded whole either way. Worth it
against the alternative: `clang-format` from the channel drags the whole of
LLVM in behind it, and comes to 1.2 GB.

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

### Installing a toolchain

When the machine has nothing suitable, Pickup can fetch one. `search` shows
what is on offer for this host, newest first; release candidates are never
listed:

```
$ pickup search clang
VERSION  SIZE  ASSET
22.1.8   1.8G  LLVM-22.1.8-Linux-X64.tar.xz
22.1.7   1.8G  LLVM-22.1.7-Linux-X64.tar.xz
21.1.8   1.9G  LLVM-21.1.8-Linux-X64.tar.xz
```

`--version` narrows it, and may name fewer components than the version has:
`21` means any 21, `20.1.5` means exactly that one.

```sh
pickup search clang --version 21
pickup install clang                      # the newest stable
pickup install clang --version 20.1.5
pickup install clang --dry-run            # resolve it, download nothing
pickup search clang --refresh             # ask the source again
```

The list of published releases is cached for an hour, because the
unauthenticated GitHub API allows 60 requests in one. `--refresh` throws that
away and asks again, for when a release lands and you would rather not wait.

Everything Pickup writes lives under `~/.pickup` (or `$PICKUP_HOME`), so
installing never asks for administrator rights:

```
~/.pickup/
  cache/                   the probed inventory and the release index
  downloads/               archives in flight, removed once installed
  toolchains/
    clang-22.1.8-x86_64-unknown-linux-gnu/
```

`cache/` and `downloads/` can be deleted at any time; the cost is a rescan, not
a wrong answer. `toolchains/` is what the downloads were for, so deleting the
whole of `~/.pickup` takes gigabytes of installed compilers with it.

Installed toolchains join the same inventory as the system ones, and `list`,
`show` and `resolve` treat them no differently.

The archive is checked against the sha256 the source published before anything
is unpacked, and the install is assembled under a temporary name and moved into
place only once it is complete, so an interrupted one leaves nothing that looks
finished.

### Only what gets used

An LLVM release unpacks to **11.29 GB**, of which the compiler is 259 MB. The
rest is MLIR, Flang, lldb, the refactoring tools, and gigabytes of static
libraries for people linking against LLVM itself. Pickup installs the compiler,
a linker, the builtin headers, the runtime and libc++ — **about 550 MB**:

```
$ pickup install clang
downloading clang 22.1.8  [█████████████████████████]  100%  1.8G/1.8G
verifying clang 22.1.8    [█████████████████████████]  100%  1.8G/1.8G
preparing the extraction  /
extracting clang 22.1.8   \  550M extracted
probing installed toolchain                        12 features
✓ clang 22.1.8 installed in ~/.pickup/toolchains/clang-22.1.8-x86_64-unknown-linux-gnu (550M)
```

Every stage says what it is doing, because each of them takes long enough on a
gigabyte to look like a hang otherwise: hashing the archive is forty seconds,
and tar spends another stretch decompressing its way to the parts that were
asked for before it writes anything at all.

The rest is never written to disk, not written and then deleted: the archive
streams past and only matching entries are unpacked. Installing does not need
11 GB free.

That selection is a guess about someone else's layout, so it is not trusted.
Once unpacked, the compiler is asked to compile — the count of features above is
that answer — and if it proves nothing, the release is unpacked in full instead
of leaving a toolchain that cannot build. `--full` skips the selection entirely.

```
$ pickup install clang --dry-run
version  22.1.8
asset    LLVM-22.1.8-Linux-X64.tar.xz
url      https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/LLVM-22.1.8-Linux-X64.tar.xz
size     1.8G (1938859476 bytes)
sha256   df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384
```

LLVM only began publishing digests recently. For a release that has none there
is nothing to check against, so Pickup refuses it rather than claim it
verified, and says what would override that:

```
$ pickup install clang --version 18.1.8
✗ clang 18.1.8: this release publishes no sha256 digest
  nothing was downloaded
  re-run with --allow-unverified to install it anyway
```

Downloads need `curl` and unpacking needs `tar`, both of which ship with
Windows 10 and later, macOS, and mainstream Linux. Neither is linked into
Pickup; it still builds against libc alone.

### A GCC, from conda-forge, without conda

The GNU project publishes no binaries, so a GCC comes from conda-forge — read
straight from the channel. There is no client to install, no environment to
activate, and no Python.

```sh
pickup search gcc
pickup install gcc --version 12
pickup install gcc --version 16 --dry-run   # the closure, nothing downloaded
```

Both report while they wait, for the same reason `search clang` does:

```
$ pickup search gcc
fetching the conda-forge listing  \

$ pickup install gcc --version 16
resolving libstdcxx-devel_linux-64  /
```

Working out a closure is one request per package — a dozen or more — and there
is no transfer to measure against, so it says which package it is asking about
rather than spinning mutely. All of it goes to stderr, so
`pickup search gcc --format toml > gcc.toml` still gets a file with nothing in
it but TOML.

A compiler there is not one package but a closure of them: `gxx` at 16.1.0
pulls in nineteen, from `gcc_impl_linux-64` at 81 MB down to the sysroot and
the development halves of libstdc++. Install the first alone and nothing
compiles, so Pickup gathers all of it or none:

```
$ pickup install gcc --version 16 --dry-run
packages 19
size     184M
  gxx                       16.1.0   hd47ba16_1     28K
  gcc_impl_linux-64         16.1.0   h5fcb69b_1     81M
  libstdcxx-devel_linux-64  16.1.0   h41cdd0d_101   21M
  …
```

**This is not a dependency solver, and Pickup does not become a package manager
by having it.** There is no backtracking and no search: the graph of one
toolchain is small and its constraints are already consistent, because the
channel built those packages against each other. It is a walk — take the newest
build satisfying each requirement, follow what it names, stop when nothing new
appears. When a requirement cannot be met the walk stops and says which one,
and nothing is downloaded.

Three things about the channel are worth knowing, because each of them is a way
to get this wrong:

- `repodata.json` for one subdirectory is **432 MB**. It is never fetched. The
  per-package endpoint answers in about a megabyte, with the dependencies of
  every build.
- That endpoint publishes md5 and not sha256. The per-build one publishes
  sha256, so it is asked for each package actually being installed — which is
  what keeps the rule that nothing is installed without a digest.
- The development halves of the compilers are published as **noarch**, despite
  names like `libstdcxx-devel_linux-64`. Read only the host's own subdirectory
  and you find the compiler and none of what it compiles against.

A `.conda` package is a zip holding two zstd tarballs, and the entries are
*stored* rather than compressed again — so reaching one means walking a few
headers and copying a range of bytes. No zip library is linked and no `unzip`
is needed; the tarball goes to the same `tar` as everything else.

Unpacking is only half of it. A package is built under a long placeholder path,
and files that had to record where they were built still carry it, so the build
prefix is rewritten to the real one. Binaries keep their length — an ELF is full
of offsets into itself — and the difference is made up with NUL bytes **at the
end of the string**, not straight after the prefix. That distinction is not
academic: a linker carries its own script embedded as text, and padding in the
middle of `SEARCH_DIR("=<prefix>/lib")` loses the closing quote and leaves a
linker that will not parse its own script.

Which is exactly why what comes out is not trusted. A closure that unpacked is
not a toolchain until it has compiled, linked and **run** a program:

```
$ pickup install gcc --version 16
probing installed toolchain                        12 features
✓ gcc 16.1.0 installed in ~/.pickup/toolchains/gcc-16.1.0-x86_64-conda-linux-gnu (990M)
```

Feature counts alone would not have caught that linker: those probes avoid
headers on purpose, so all twelve passed against a toolchain that could not
link `#include <stdio.h>`. Compiling something real is the check that matters.

### Output for machines

Every command that produces data takes `--format toml`. TOML because Molto
already parses it: consuming Pickup adds no parser to the consumer.

```
$ pickup resolve --lang c++ --format toml
[compiler]
path = "~/.pickup/toolchains/clang-22.1.8-x86_64-unknown-linux-gnu/bin/clang++"
c_path = "~/.pickup/toolchains/clang-22.1.8-x86_64-unknown-linux-gnu/bin/clang"
cxx_path = "~/.pickup/toolchains/clang-22.1.8-x86_64-unknown-linux-gnu/bin/clang++"
vendor = "clang"
version = "22.1.8"
target = "x86_64-unknown-linux-gnu"

[cxx]
stdlib = "libstdc++"
compile_flags = ["--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11"]
link_flags = ["--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11"]
runtime_dirs = []
```

### The flags are part of the answer

A compiler is not a command; it is a command plus whatever it has to be told
before it produces a program that runs. On Linux a Clang has to be told which
C++ standard library to use, and when that library is its own, where the loader
will find it afterwards. Answer with a path alone and the caller is left to
work the rest out, which is how a build ends up hard-coding flags for the
machine it was written on.

So `resolve` publishes the recipe, and like everything else here it is proven
rather than assumed: candidates are tried in order and the first that compiles,
links **and runs** is the one published.

libstdc++ is tried before libc++ deliberately. The two are not ABI compatible,
and libstdc++ is what everything else on the machine was built against; falling
back to the compiler's own library makes a toolchain self-contained at the cost
of no longer linking against the system's C++ libraries. `--stdlib` forces one
when the project has a constraint Pickup cannot see:

```
$ pickup resolve --lang c++ --stdlib libc++ --format toml
[cxx]
stdlib = "libc++"
compile_flags = ["-stdlib=libc++", "--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11"]
link_flags = ["-stdlib=libc++", "-Wl,-rpath,~/.pickup/toolchains/clang-22.1.8-x86_64-unknown-linux-gnu/lib/x86_64-unknown-linux-gnu", …]
runtime_dirs = ["~/.pickup/toolchains/clang-22.1.8-x86_64-unknown-linux-gnu/lib/x86_64-unknown-linux-gnu"]
```

`runtime_dirs` is there because linking is not running. Without that `-rpath`
the link exits zero and the program dies on `libc++.so.1: cannot open shared
object file` — a toolchain that passes every check and builds nothing, which is
the one thing Pickup must never report as success.

`install` leaves the same recipe in a `clang++.cfg` beside the driver, which
Clang reads by itself, so a toolchain works when invoked directly too. That is
a convenience; the contract is the TOML, which also covers system compilers
that cannot be written to.

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
