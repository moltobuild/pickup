# Pickup

The toolchain manager of the [Molto](../molto) ecosystem. See [`spec.md`](spec.md)
for the design; the RFC process it inherits lives in [Molto](../molto/rfcs/).

Pickup answers one question truthfully: **which compilers exist on this machine,
and what can each of them actually do?**

That is what lets a project manifest say *what it needs* instead of *which
machine it was written on*.

## Install

A static x86-64 Linux binary is attached to every
[release](https://github.com/moltobuild/pickup/releases). It carries no glibc
requirement, which matters here more than most: the machines that most want a
newer compiler are the ones with an older libc.

```sh
base=https://github.com/moltobuild/pickup/releases/latest/download
curl -fsSLO $base/SHA256SUMS
curl -fsSLO $base/pickup-0.3.3-x86_64-linux
sha256sum --check --ignore-missing SHA256SUMS
sudo install pickup-0.3.3-x86_64-linux /usr/local/bin/pickup
```

```sh
pickup list             # what this machine already has
pickup install clang    # or fetch one, under ~/.pickup, no root
```

Installing anything needs `curl`, `tar` and `zstd` on the `PATH`;
`pickup doctor` says so when one is missing. Licensed Apache-2.0, see
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).

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
- GNU Make, for the bootstrap
- `curl`, `tar` and `zstd` at runtime, for `install`

## Build

```sh
make build        # produces build/pickup
molto build       # produces build/debug/pickup
```

Both work. `Project.toml` describes the build completely, so [molto](../molto)
builds pickup exactly as it builds anything else. The Makefile remains because
the first pickup has to be compiled by something that does not need pickup to
find a compiler.

## Test

```sh
make test
molto test
```

## Usage

```sh
pickup list                # the inventory
pickup show <name>         # one toolchain, feature by feature
pickup tools               # the formatter and linter this machine has, and where
pickup doctor              # what stops this machine from building, and what fixes it
pickup scan                # re-probe everything and rewrite the cache
pickup resolve [options]   # the best toolchain for a set of requirements
pickup search [name]       # what the registry publishes, or one name's versions
pickup install <name>      # download and install one
pickup uninstall <name>    # remove one pickup installed
pickup default [name]      # show or set the one resolve should prefer
```

`search` and `install` take any name the registry publishes: a toolchain such as
`clang` or `gcc`, or a tool such as `clang-format` or `clang-tidy`. `search` with
no name lists all of them.

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

The first `list` on a machine has to probe, and probing is every compiler on it
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
asking the registry  \
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

Which is worth spelling out for the run above: a missing formatter is a `✗`, so
that machine makes `pickup doctor` exit 1. In CI, that is the difference between
a gate and a report.

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

Remedies are named, never applied — with one exception, and it stays on
Pickup's own side of the line. A toolchain's `.cfg` records a decision made
when it was installed, and nothing revalidates it: install `g++-12` and the
file may go on naming a GCC that is no longer the right one, while `resolve`,
which works the recipe out afresh every time, says something else. So `doctor`
puts it back in step, and says it did:

```
  ✓ clang@22.1.8 reconfigured - now uses /usr/lib/gcc/x86_64-linux-gnu/12
```

Repairing a GCC in `/usr` is the reader's decision; keeping a file under the
pickup home in step with reality is not a decision at all. Three conditions
keep it there: only toolchains Pickup installed, only files Pickup wrote —
recognised by the line they open with, so one edited by hand is left exactly as
it is — and only when the recipe actually differs.

Examining means asking every compiler to build, link and run something, several
times over, so it says how far it has got:

```
examining toolchains  [████████████████████░░░░░]   83%  5/6
```

### The tools a project is worked on with

A machine that compiles is not a machine that can be worked on. `doctor` looks
for a formatter and a linter — `clang-format`, `clang-tidy`, `cppcheck` — on
PATH and in what Pickup installed, and asks each one to identify itself: a file
with the right name that does not answer is a file, not a tool.

`cppcheck` is found when the system provides it and cannot be installed: the
registry does not publish it, and `clang-tidy` covers the linter `doctor` asks
for.

```sh
pickup install clang-format
pickup install clang-tidy
```

Both come from the registry, down the same path as a compiler. What differs is
the test they have to pass before being adopted: there is nothing here to
compile, so it is that the binary the registry named runs and answers.
They land under `~/.pickup/tools/`, apart from the toolchains, because nothing
resolves against them.

`pickup tools` says which ones are there and, in TOML, where:

```
$ pickup tools
KIND       NAME          VERSION                      SOURCE
formatter  clang-format  clang-format version 22.1.8  pickup
linter     clang-tidy    LLVM version 22.1.8          pickup
```

```toml
$ pickup tools --format toml
[[tool]]
kind = "formatter"
name = "clang-format"
path = "~/.pickup/toolchains/clang-22.1.8-…/bin/clang-format"
version = "clang-format version 22.1.8"
source = "pickup"
```

**`doctor` says whether one is missing; `tools` says which are there and what
to invoke.** The same division as `doctor` and `list` for compilers, and for
the same reason: a caller reading paths out of a report of problems is reading
the wrong document. Only the TOML carries the path, because that is what a
build needs and what would push every other column off a line meant for a
person.

The version is read from whichever line carries it. clang-format answers with
its version followed by the URL it was built from; clang-tidy opens with a
banner and puts the version underneath. Taking the first line would report the
linter as "LLVM".

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

When the machine has nothing suitable, Pickup fetches one. Everything comes from
one registry, which publishes toolchains and tools under coordinates of its own:
a name, a version, and the target it was built for.

`search` with no name lists what there is:

```
$ pickup search
NAME          KIND       VERSION  TARGETS
clang         toolchain  19.1.6   linux-x86_64
gcc           toolchain  16.1.0   linux-x86_64
clang-format  tool       19.1.6   linux-x86_64
clang-tidy    tool       19.1.6   linux-x86_64
```

With a name, the versions of it that can be installed here:

```
$ pickup search clang
VERSION  SIZE  TARGET        STATUS
19.1.6   44M   linux-x86_64
```

`--version` narrows it, and may name fewer components than the version has:
`19` means any 19, `19.1.6` means exactly that one.

```sh
pickup search clang --version 19
pickup install clang                      # the newest offered
pickup install clang --version 19.1.6
pickup install clang --dry-run            # resolve it, download nothing
pickup search clang --refresh             # ask the registry again
```

The catalogues are cached for an hour, because `search` is the kind of command
people run twice in a row. `--refresh` throws that away and asks again.

A name nobody publishes comes back as a list rather than as an error:

```
$ pickup install clan
pickup: nothing is published as 'clan'
  did you mean:
    clang (toolchain 19.1.6)
    clang-format (tool 19.1.6)
    clang-tidy (tool 19.1.6)
```

That is not politeness. Downloads go through `curl -f`, which reports a 404 and
a dead network the same way, so a name is resolved against the catalogues
already fetched instead of against a failure that cannot be told apart from an
outage.

Everything Pickup writes lives under `~/.pickup` (or `$PICKUP_HOME`), so
installing never asks for administrator rights:

```
~/.pickup/
  cache/                   the probed inventory and the registry's catalogues
  downloads/               archives in flight, removed once installed
  toolchains/
    clang-19.1.6-x86_64-unknown-linux-gnu/
  tools/
    clang-format-19.1.6/
```

`cache/` and `downloads/` can be deleted at any time; the cost is a rescan, not
a wrong answer. Tools live apart from toolchains because nothing resolves
against them.

Installed toolchains join the same inventory as the system ones, and `list`,
`show` and `resolve` treat them no differently.

### What arrives, and what it has to prove

```
$ pickup install clang
downloading clang 19.1.6  [█████████████████████████]  100%  44M/44M
verifying clang 19.1.6    [█████████████████████████]  100%  44M/44M
extracting clang 19.1.6   \  209M extracted
probing installed toolchain                        12 features
✓ clang 19.1.6 installed in ~/.pickup/toolchains/clang-19.1.6-x86_64-unknown-linux-gnu (209M)
```

Every stage says what it is doing, because each takes long enough to look like a
hang otherwise.

**Nothing is installed without being checked.** The registry states a sha256 for
every artifact, so one that arrives without a digest is a broken answer and is
dropped while reading, not carried to a point where something decides whether to
verify it. There is no flag to switch this off, because there is no longer a
case it would serve.

**Nothing is trusted for having unpacked.** A compiler has to compile, link and
*run* a program before it is adopted; the feature count above is part of that
answer. A tool has nothing to compile, so what stands in for it is that the
binary the registry named actually answers. Whatever fails leaves nothing
installed.

**Nothing published about it is obeyed.** The registry says which flags an
artifact was built with, and Pickup shows them and then works out its own. Those
metadata describe the machine that built the artifact; the flag that makes a
compiler work here names a directory on *this* machine. A Clang that has to be
pointed at a particular GCC installation, or a GCC that carries a newer
libstdc++ than the system and cannot run what it links until the loader is told
where it is, are both discovered by trying — never by reading.

```
$ pickup install clang --dry-run
name     clang
kind     toolchain
version  19.1.6
target   linux-x86_64
format   tar.zst
size     44M (46459314 bytes)
sha256   f221669aeffba6a6a77c43387ecd098aa160105591a4249671be159bdf9fd8b9
url      https://molto-registry.molto-build.workers.dev/v1/toolchains/clang/19.1.6/linux-x86_64/download
about    LLVM C and C++ compiler, shipping libc++ so C++ needs nothing from the host
```

How a blob is packed is read from `format`, never guessed from the URL. Today
that is `tar.zst` throughout, which makes `zstd` a requirement alongside `curl`
and `tar` — `pickup doctor` says so when it is missing, because without it
nothing can be installed at all.

A version the registry has **withdrawn** stays listed and stops being offered:

```
$ pickup install clang
✗ clang 19.1.5: withdrawn by the registry, and not to be used for new builds
  nothing was downloaded
  name the whole version to install it anyway: pickup install clang --version 19.1.5
```

Withdrawn means "not for new builds", not "gone". Naming the whole version
installs it, because something already installed from it has to stay
explicable.

The install is assembled under a temporary name inside the directory it will end
up in, and renamed into place only once it is complete — one filesystem, so the
rename is atomic and an interrupted install leaves nothing that looks finished.

### Where it installs from

```
1. $PICKUP_REGISTRY_URL
2. registry = "…" in ~/.pickup/config.toml
3. the built-in default
```

Pickup reads that key and never writes it. Pointing it somewhere else decides
which registry to trust, and a command able to change it would be a command able
to redirect every future install.


### Packing what it installs

The registry serves what someone put in it, and `scripts/pack_toolchain.sh` is
how that is done reproducibly:

```sh
scripts/pack_toolchain.sh <prefix> <name> <version> <target> [outdir]
PACK_SYSROOT=<dir> scripts/pack_toolchain.sh …   # ship this sysroot instead
```

It writes the `tar.zst` and the `recipe.toml` that go up in the two publishing
requests. What it is really doing is leaving things out, because that is the
whole reason this registry exists — the numbers from this machine:

| Toolchain    | Upstream | Packed | Downloaded |
| ------------ | -------- | ------ | ---------- |
| clang 22.1.8 | 735 MB   | 276 MB | 68 MB      |
| gcc 12.3.0   | 856 MB   | 238 MB | 64 MB      |
| gcc 15.3.0   | 717 MB   | 313 MB | 81 MB      |

Every exclusion is argued in the script, next to where it is made. Two of them
are traps rather than choices, and belong here as well:

- **`bin/` sits at the root of the archive.** Extraction uses
  `--strip-components=0`, so one extra directory level is a toolchain pickup
  unpacks and then cannot find.
- **The sysroot decides who can run the output.** A gcc packed with a glibc
  newer than the host's compiles, links and runs `puts("ok")` — and then
  produces a real program that dies at startup with `GLIBC_2.38 not found`.
  The script compares the shipped C library against the host's and refuses to
  pack rather than hand anyone a compiler like that. `PACK_SYSROOT` names an
  older one; glibc 2.28 is the oldest that still has `_DEFAULT_SOURCE`, which
  molto's own sources need.

Before writing the archive it compiles, links and **runs** a program in both C
and C++ out of the trimmed tree, for the reason `spec.md` gives: counting
features proves nothing, because the probes avoid headers on purpose and a
compiler with a broken sysroot passes every one of them.


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
stdlib = "libc++"
compile_flags = ["-stdlib=libc++", "--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11"]
link_flags = ["-stdlib=libc++", "-Wl,-rpath,~/.pickup/toolchains/clang-19.1.6-x86_64-unknown-linux-gnu/lib/x86_64-unknown-linux-gnu", …]
runtime_dirs = ["~/.pickup/toolchains/clang-19.1.6-x86_64-unknown-linux-gnu/lib/x86_64-unknown-linux-gnu"]
```

`id` names the toolchain, as `list` and `default` do. Asking with `--std` adds
one more key, `std_flag = "-std=c2x"`, so the caller passes the flag Pickup
verified rather than the one it assumed:

```
$ pickup resolve --std c2x --format toml
[compiler]
…
std_flag = "-std=c2x"
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

Which library it settles on depends on who owns the compiler.

For one the host owns, libstdc++ is tried before libc++ deliberately. The two
are not ABI compatible, and libstdc++ is what everything else on the machine was
built against; falling back to the compiler's own library makes a toolchain
self-contained at the cost of no longer linking against the system's C++
libraries.

For a toolchain Pickup installed it is the other way round: what it brought
inside its own prefix beats what the host lends it. That toolchain was built
elsewhere and borrows a libstdc++ only by accident of where it was unpacked,
which would make one `pickup install` a different compiler on every machine.
Ownership is read from the path, so nothing outside `~/.pickup/toolchains`
changes behaviour.

`--stdlib` forces one when the project has a constraint Pickup cannot see —
linking against a system-packaged C++ library, for instance, which is built
against libstdc++ and will not resolve symbols compiled against libc++:

```
$ pickup resolve --lang c++ --stdlib libstdc++ --format toml
[cxx]
stdlib = "libstdc++"
compile_flags = ["--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11"]
link_flags = ["--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11"]
runtime_dirs = []
```

`runtime_dirs` is there because linking is not running. Without that `-rpath`
the link exits zero and the program dies on `libc++.so.1: cannot open shared
object file` — a toolchain that passes every check and builds nothing, which is
the one thing Pickup must never report as success.

`install` leaves the same recipe in a `clang++.cfg` beside the driver, which
Clang reads by itself, so a toolchain works when invoked directly too. That is
a convenience; the contract is the TOML, which also covers system compilers
that cannot be written to.

### Picking which one wins

`resolve` answers with the newest toolchain that can actually build. That is a
reasonable default and it is pickup's, not yours — on a machine with a very new
Clang it will be chosen every time, whether or not that is the one you build
with.

`default` is how you say otherwise:

```
$ pickup default gcc@12.3.0-conda
✓ Default toolchain is now gcc@12.3.0-conda

$ pickup resolve
cc gcc 12.3.0 (/home/you/.pickup/toolchains/gcc-12.3.0-x86_64-conda-linux-gnu/bin/cc)
```

It is a preference, not a constraint. Ask for something it cannot do and you
get an answer anyway, plus a line on stderr saying why it was passed over:

```
$ pickup resolve --vendor clang
pickup: the default gcc@12.3.0-conda cannot serve this request; chose clang@22.1.8
clang clang 22.1.8 (/home/you/.pickup/toolchains/clang-22.1.8-.../bin/clang)
```

What gets stored is the resolved identity, never what you typed: `gcc@latest`
recorded as written would quietly mean a different compiler the next time
anything was installed. `pickup default` on its own reports the current one,
`--clear` forgets it, and `list` marks it.

The preference lives in `~/.pickup/config.toml`, beside the cache rather than
inside it. Clearing the cache costs a rescan; it must not cost a decision.

### Removing one

```
$ pickup uninstall gcc@12.3.0-conda
Remove gcc@12.3.0-conda
  /home/you/.pickup/toolchains/gcc-12.3.0-x86_64-conda-linux-gnu  (845M)
This cannot be undone. Continue? [y/N]
```

Only toolchains pickup installed. One that was already on the machine belongs
to the package manager, and pickup says so rather than touching it. A name that
matches more than one is refused outright — deleting something other than what
was meant is the failure worth preventing, so `pickup uninstall gcc` on a
machine with four of them lists them and stops.

The prompt appears where there is someone to answer it — that is, when stdin is
a terminal. In a pipe there is nobody, so the removal proceeds unasked, with or
without `--yes`: a script that named a toolchain outright meant it. `--yes`
skips the question anywhere.

### Exit codes

| Code | Meaning                            |
|------|------------------------------------|
| 0    | Success                            |
| 1    | Operation failed                   |
| 2    | Invalid usage                      |
| 3    | No toolchain satisfies the request |
| 4    | Command not implemented yet        |

Code 4 is reserved and nothing returns it today: every command in the table
above is implemented.

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
