#!/usr/bin/env bash
#
# Pack an llvm-mingw prefix into the artifact the registry serves and pickup
# installs. A sibling of pack_toolchain.sh, deliberately not an extension of
# it: the Windows question differs from the Linux one at four points that no
# flag would hide, and each is answered here with the reason beside it.
#
#   1. A driver that emits for Windows is not `bin/clang`. llvm-mingw ships a
#      clang whose default target is the *host*, plus prefixed drivers that
#      select the Windows target through a config file. Publishing bin/clang
#      would publish a toolchain that installs, resolves, compiles, and emits
#      ELF -- the "meaning" failure of RFC-0017, one layer out.
#   2. The .cfg files are the toolchain. pack_toolchain.sh deletes every *.cfg
#      because a conda one embedded the packer's sysroot; here they carry the
#      `-target` line the prefixed drivers work by, and deleting them leaves a
#      prefix that compiles for the wrong platform. See drop_host_configuration.
#   3. Nothing built here runs here. Verification goes through wine, which is
#      the only way to answer "does the program this toolchain produced
#      actually start" without a Windows machine.
#   4. There is no rpath on Windows. A C++ program links against libc++.dll and
#      libunwind.dll and finds them beside the executable or on PATH, so the
#      runtime directory has to be named in the recipe rather than baked into
#      the link.
#
# Usage:
#   scripts/pack_windows_toolchain.sh <prefix> <name> <version> <host> [outdir]
#
#   prefix   an unpacked llvm-mingw release, the directory holding bin/
#   name     the registry name: llvm-mingw
#   version  the version clang reports, exactly
#   host     where the toolchain RUNS, in pickup's platform literal:
#            linux-x86_64 or windows-x86_64. Not what it emits for -- that is
#            the triple, and it is read off the driver.
#   outdir   where the archive and the recipe are written (default: .)
#
# Environment:
#   WINE     the wine loader to verify with. Required for a windows-x86_64
#            host, where the compiler itself has to run under it.

set -euo pipefail

readonly ZSTD_LEVEL=19
readonly GZIP_LEVEL=9

# How a host's artifact is packed.
#
# zstd for a Linux host: it decompresses several times faster, and every Linux
# that can install anything can install zstd.
#
# gzip for a Windows host, and gzip only. This was tar.xz on the belief that
# the tar Windows ships opens gzip, bzip2, xz and lzma; of that list only the
# first is true. That tar is bsdtar with libarchive linked against zlib alone
# -- `tar --version` prints the libraries it has and names no other -- and
# every codec that is not gzip it hands to an outside program. On Windows that
# path fails twice over: the program is not installed, and when it is,
# libarchive fills its stdin while waiting on its stdout and the pair deadlocks
# past a pipe buffer of compressed input. Measured on the tar Windows 10 ships:
# 816 bytes of xz lists fine, 128 KB of xz never returns.
#
# It costs bytes and it is worth them. The llvm-mingw 23.1.0 artifact is 73.6 MB
# packed with xz -9 and 127.9 MB packed with gzip -9, from 488.9 MB unpacked.
# Fifty-four extra megabytes, once, buys an install that works in cmd, in
# PowerShell and in a Unix shell -- and on Windows the shell does not even get
# a say: pickup starts tar through CreateProcess, which searches the system
# directory before PATH, so C:\Windows\System32\tar.exe is what runs no matter
# what the user has installed or which shell they typed in.
packing_for() {
    case "$1" in
    windows-*) printf 'tar.gz\n' ;;
    *) printf 'tar.zst\n' ;;
    esac
}
readonly C_STANDARDS="c89 c99 c11 c17 c2x c23 c2y"
readonly CXX_STANDARDS="c++11 c++14 c++17 c++20 c++23 c++26"

# The one architecture published. llvm-mingw carries six, and five of them are
# 210 MB that an x86-64 build never opens.
readonly KEEP_ARCH="x86_64"

die() {
    printf 'pack_windows_toolchain: %s\n' "$1" >&2
    exit 1
}

note() {
    printf '  %s\n' "$1" >&2
}

require_tools() {
    local tool
    for tool in tar zstd gzip file sha256sum; do
        command -v "$tool" >/dev/null || die "$tool is needed and not on the PATH"
    done
    tar --help 2>/dev/null | grep -q zstd || die "this tar cannot open zstd archives"
}

# On Windows a suffix is a permission (RFC-0017): what says a file may be run
# is that it is called .exe. So the host decides how every executable in this
# script is spelled, and it is asked once here rather than guessed at each use.
suffix_for_host() {
    case "$1" in
        windows-*) printf '.exe' ;;
        *) printf '' ;;
    esac
}

# --------------------------------------------------------------- staging

# Everything that is not the published architecture. Each entry is a directory
# or a filename fragment, and the two are matched differently below.
prune_other_architectures() {
    local stage="$1"
    local arch
    for arch in aarch64 armv7 arm64ec i686; do
        rm -rf "${stage:?}/$arch-w64-mingw32" "${stage:?}/$arch-w64-mingw32uwp"
        find "$stage/bin" -maxdepth 1 -name "$arch-*" -delete 2>/dev/null || true
        rm -f "$stage/bin/$arch-"*.cfg
    done

    # compiler-rt for every Windows architecture, trimmed to the one published.
    local runtime
    for runtime in "$stage"/lib/clang/*/lib/windows; do
        [ -d "$runtime" ] || continue
        find "$runtime" -maxdepth 1 -type f ! -name "*$KEEP_ARCH*" -delete
    done
}

# The development kit that rides along with a compiler and is not one.
#
#   lldb, liblldb        a debugger, 17 MB, and its own `tool` artifact if it
#                        is ever wanted
#   clang-tidy, clangd   the registry publishes these separately already
#   libscanbuild/libear  the static analyser's python harness
#   llvm-* utilities     kept only where a driver invokes them: the prefixed
#                        ar, ranlib, strip, dlltool and windres are part of a
#                        link, and llvm-objdump and the rest are not
prune_development_kit() {
    local stage="$1" suffix="$2"
    rm -rf "$stage/lib/libscanbuild" "$stage/lib/libear" "$stage/share/scan-build" \
        "$stage/share/scan-view"
    rm -f "$stage"/lib/liblldb* "$stage"/lib/liblldbIntelFeatures*

    local tool
    for tool in lldb lldb-argdumper lldb-dap lldb-server clangd clang-tidy \
        clang-scan-deps clang-scan-deps-wrapper analyze-build intercept-build \
        scan-build scan-view git-clang-format llvm-symbolizer llvm-profdata \
        llvm-cov llvm-cxxfilt llvm-readobj llvm-objdump llvm-size llvm-strings; do
        rm -f "$stage/bin/$tool$suffix" "$stage/bin/$tool"
    done
    find "$stage/bin" -maxdepth 1 -name "*-w64-mingw32-clang-scan-deps*" -delete 2>/dev/null || true
}

# What pack_toolchain.sh calls drop_configuration, inverted and narrowed.
#
# That script deletes every *.cfg because a conda clang shipped one naming the
# packer's own --sysroot, which then travelled into every install. The hazard
# is real and the remedy does not transfer: llvm-mingw's .cfg files hold
#   -target x86_64-w64-mingw32
#   @mingw32-common.cfg
# and nothing else -- no absolute path, nothing about the machine that built
# them. They are how a prefixed driver knows what to emit, and a prefix without
# them is a clang that compiles for Linux under a Windows name.
#
# So the rule becomes the narrow one it should always have been: delete a
# configuration file that names a path outside the prefix, and keep the rest.
drop_host_configuration() {
    local stage="$1"
    local config
    while IFS= read -r config; do
        if grep -qE '(^|[[:space:]=])/' "$config" 2>/dev/null; then
            note "dropping $(basename "$config"): it names an absolute path"
            rm -f "$config"
        fi
    done < <(find "$stage" -name '*.cfg')
}

# ELF is stripped with the host's strip; PE is stripped with the toolchain's
# own llvm-strip, because the host's binutils does not know the format. Where
# neither applies the binary travels unstripped, which costs bytes and breaks
# nothing.
strip_binaries() {
    local stage="$1" suffix="$2"
    local stripper="$stage/bin/llvm-strip$suffix"
    local binary kind

    find "$stage/bin" "$stage/lib" -maxdepth 1 -type f 2>/dev/null | while read -r binary; do
        kind="$(file -b "$binary")"
        case "$kind" in
            *ELF*) strip --strip-unneeded "$binary" 2>/dev/null || true ;;
            *PE32*) [ -x "$stripper" ] && run_native "$stripper" --strip-unneeded "$binary" \
                2>/dev/null || true ;;
        esac
    done
}

# --------------------------------------------------------------- running

# Run a program that belongs to the packed toolchain. On a linux host that is
# just running it; on a windows host every binary in the prefix is a PE and
# has to go through wine, compiler included.
run_native() {
    if [ "$NEEDS_WINE" = yes ]; then
        WINEDEBUG=-all "$WINE" "$@"
    else
        "$@"
    fi
}

# Run a program the toolchain produced. Always a PE, so always wine, and always
# with the runtime directory on PATH: libc++.dll and libunwind.dll are found
# beside the executable or not at all.
run_emitted() {
    local runtime="$1"
    shift
    WINEDEBUG=-all WINEPATH="$runtime" "$WINE" "$@"
}

require_wine() {
    [ -n "${WINE:-}" ] || die "WINE names the loader that verification runs through, and is unset"
    [ -x "$WINE" ] || die "WINE=$WINE is not executable"
    note "verifying through $("$WINE" --version 2>/dev/null || basename "$WINE")"
}

# --------------------------------------------------------------- the drivers

# The prefixed driver, which is the one that emits for Windows. Asked of the
# tree rather than composed, so a release that renames one is a failure here
# and not a wrong answer in the recipe.
find_driver() {
    local stage="$1" triple="$2" tool="$3" suffix="$4"
    local path="bin/$triple-$tool$suffix"
    [ -e "$stage/$path" ] || die "$stage has no $path, which is the driver that emits for Windows"
    printf '%s' "$path"
}

# What the driver says it emits for. clang normalises `mingw32` to
# `windows-gnu`, so this answers x86_64-w64-windows-gnu and that -- not the
# spelling in the directory name -- is what pickup will match a --target
# against.
detect_triple() {
    run_native "$1" -dumpmachine | tr -d '\r'
}

# --------------------------------------------------------------- verifying

# Compile, link and *run* a program in both languages, exactly as
# pack_toolchain.sh does and for the same reason: counting features proves
# nothing, and an archive that cannot do this would install and then break a
# real build. The difference is only that running happens under wine.
verify_stage() {
    local stage="$1" c_driver="$2" cxx_driver="$3" runtime="$4"
    local work
    work="$(mktemp -d)"

    printf '#include <stdio.h>\nint main(void){puts("ok");return 0;}\n' > "$work/probe.c"
    printf '#include <iostream>\nint main(){std::cout<<"ok\\n";return 0;}\n' > "$work/probe.cpp"

    run_native "$stage/$c_driver" "$work/probe.c" -o "$work/probe_c.exe" \
        || die "the trimmed toolchain cannot compile C"
    run_emitted "$runtime" "$work/probe_c.exe" > /dev/null \
        || die "the C program the trimmed toolchain built does not run"
    note "C compiles, links and runs"

    run_native "$stage/$cxx_driver" "$work/probe.cpp" -o "$work/probe_x.exe" \
        || die "the trimmed toolchain cannot compile C++"
    run_emitted "$runtime" "$work/probe_x.exe" > /dev/null \
        || die "the C++ program the trimmed toolchain built does not run"
    note "C++ compiles, links and runs against the bundled libc++"

    rm -rf "$work"
}

# The artifact has to be a Windows toolchain, and `file` is what says so
# without trusting a name. Checked after the trim, because a prune that took
# the wrong tree is exactly the failure this catches.
verify_emits_pe() {
    local stage="$1" c_driver="$2"
    local work
    work="$(mktemp -d)"
    printf 'int main(void){return 0;}\n' > "$work/probe.c"
    run_native "$stage/$c_driver" "$work/probe.c" -o "$work/probe.exe" 2>/dev/null || true
    file -b "$work/probe.exe" 2>/dev/null | grep -q 'PE32+' \
        || die "the toolchain does not emit PE32+ binaries"
    note "emits PE32+ executables for MS Windows"
    rm -rf "$work"
}

# Which -std values the compiler accepts, asked rather than assumed.
accepted_standards() {
    local driver="$1" language="$2" candidates="$3"
    local work accepted="" standard separator=""
    work="$(mktemp -d)"
    printf 'int main(void){return 0;}\n' > "$work/probe.c"

    for standard in $candidates; do
        if run_native "$driver" -x "$language" "-std=$standard" -fsyntax-only \
            "$work/probe.c" 2>/dev/null; then
            accepted="$accepted$separator\"$standard\""
            separator=", "
        fi
    done
    rm -rf "$work"
    printf '%s' "$accepted"
}

# --------------------------------------------------------------- the recipe

write_recipe() {
    local out="$1" name="$2" version="$3" host="$4" triple="$5" stage="$6"
    local c_driver="$7" cxx_driver="$8" runtime="${9}" packing="${10}"

    {
        printf 'schema = 1\nform = "binary"\nkind = "toolchain"\n'
        printf 'name = "%s"\nversion = "%s"\ntarget = "%s"\n' "$name" "$version" "$host"
        # Declared, not left to the registry to assume. Everything published
        # before this was zstd and the registry's default said so; a toolchain
        # that runs on Windows is the first one that default is wrong about.
        printf 'format = "%s"\n\n' "$packing"
        printf '[toolchain]\nvendor = "clang"\ntriple = "%s"\n' "$triple"
        printf 'c_driver = "%s"\ncxx_driver = "%s"\n\n' "$c_driver" "$cxx_driver"
        printf '[toolchain.c]\nstd = [%s]\ncompile_flags = []\nlink_flags = []\n' \
            "$(accepted_standards "$stage/$c_driver" c "$C_STANDARDS")"
        printf 'runtime_dirs = ["%s"]\n\n' "$runtime"
        printf '[toolchain.cxx]\nstd = [%s]\nstdlib = "libc++"\n' \
            "$(accepted_standards "$stage/$cxx_driver" c++ "$CXX_STANDARDS")"
        printf 'compile_flags = []\nlink_flags = []\nruntime_dirs = ["%s"]\n\n' "$runtime"
        printf '[about]\ndescription = "clang %s targeting %s, running on %s"\n' \
            "$version" "$triple" "$host"
        printf 'licence = "Apache-2.0 WITH LLVM-exception"\n'
    } > "$out"
}

# --------------------------------------------------------------- main

main() {
    [ $# -ge 4 ] || die "usage: pack_windows_toolchain.sh <prefix> <name> <version> <host> [outdir]"
    local prefix="$1" name="$2" version="$3" host="$4" outdir="${5:-.}"
    [ -d "$prefix/bin" ] || die "$prefix has no bin/ directory"

    case "$host" in
        linux-x86_64) NEEDS_WINE=no ;;
        windows-x86_64) NEEDS_WINE=yes ;;
        *) die "host must be linux-x86_64 or windows-x86_64, not $host" ;;
    esac
    require_tools
    require_wine
    mkdir -p "$outdir"

    local suffix stage
    suffix="$(suffix_for_host "$host")"
    stage="$(mktemp -d)"
    trap "rm -rf '$stage'" EXIT

    note "packing $name $version from $prefix, to run on $host"
    cp -a "$prefix/." "$stage/"

    prune_other_architectures "$stage"
    prune_development_kit "$stage" "$suffix"
    drop_host_configuration "$stage"
    strip_binaries "$stage" "$suffix"

    local triple c_driver cxx_driver runtime
    c_driver="$(find_driver "$stage" "$KEEP_ARCH-w64-mingw32" clang "$suffix")"
    cxx_driver="$(find_driver "$stage" "$KEEP_ARCH-w64-mingw32" clang++ "$suffix")"
    triple="$(detect_triple "$stage/$c_driver")"
    runtime="$KEEP_ARCH-w64-mingw32/bin"
    note "the driver emits for $triple"

    note "trimmed to $(du -sh "$stage" | cut -f1), from $(du -sh "$prefix" | cut -f1)"
    verify_emits_pe "$stage" "$c_driver"
    verify_stage "$stage" "$c_driver" "$cxx_driver" "$stage/$runtime"

    # bin/ has to sit at the root: pickup extracts with --strip-components=0.
    local packing
    packing=$(packing_for "$host")
    local archive="$outdir/$name-$version-$host.$packing"
    case "$packing" in
    # -n so the blob has no build date in it: the same tree packed twice is the
    # same bytes, and the same sha256 the registry publishes.
    tar.gz) tar -C "$stage" -c -I "gzip -$GZIP_LEVEL -n" -f "$archive" . ;;
    *) tar -C "$stage" -c -I "zstd -$ZSTD_LEVEL -T0" -f "$archive" . ;;
    esac
    write_recipe "$outdir/recipe-$host.toml" "$name" "$version" "$host" "$triple" \
        "$stage" "$c_driver" "$cxx_driver" "$runtime" "$packing"

    printf '%s\n' "$archive"
    printf 'sha256: %s\n' "$(sha256sum "$archive" | cut -d' ' -f1)"
    printf 'bytes:  %s\n' "$(stat -c %s "$archive")"
    printf 'format: %s\n' "$packing"
    printf 'recipe: %s/recipe-%s.toml\n' "$outdir" "$host"
}

main "$@"
