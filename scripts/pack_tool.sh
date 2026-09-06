#!/usr/bin/env bash
#
# Pack one tool into the artifact the registry serves and pickup installs: a
# tar.gz holding the binary and whatever it needs to start, plus the
# recipe.toml that describes it.
#
# The sibling of pack_toolchain.sh and pack_windows_toolchain.sh, and the
# smallest of the three, because a tool is one program rather than a compiler
# with a sysroot. What it is really about is the second half of that sentence:
# what a program needs to start is not the same on every platform, and on
# Windows it is most of LLVM.
#
#   A Linux clang-format is 1.3 MB and links its world statically. The Windows
#   one is 78 KB and imports libLLVM-23.dll (75 MB), libclang-cpp.dll (48 MB),
#   libc++.dll and libunwind.dll. clang-tidy and clangd import exactly the
#   same four, so each of the three artifacts carries its own copy.
#
#   That duplication is not an oversight to be optimised away here. A tool is
#   installed on its own, under <PICKUP_HOME>/tools/<name>-<version>, and
#   pickup proves it by running it (install_service, tool_answers). A binary
#   that only starts when some other artifact happens to be installed beside
#   it is a binary that fails on the machine that installed only it.
#
# Usage:
#   scripts/pack_tool.sh <prefix> <name> <version> <target> [outdir]
#
#   prefix   an unpacked LLVM release, the directory holding bin/
#   name     the registry name: clang-format, clang-tidy, clangd
#   version  the version the tool reports, exactly
#   target   the platform literal pickup asks for: windows-x86_64
#   outdir   where the archive and the recipe are written (default: .)
#
# Environment:
#   WINE     the loader to verify through, when packing a Windows tool
#            somewhere that cannot run one. Unset and unnecessary on Windows.

set -euo pipefail

readonly ZSTD_LEVEL=19
readonly GZIP_LEVEL=9

# What makes the same input pack to the same bytes: a fixed entry order rather
# than the filesystem's, and nobody's uid on the way out. Worth having because
# a published coordinate is immutable — the only check anyone can make on an
# artifact later is to pack the release again and compare the digest.
readonly REPRODUCIBLY="--sort=name --owner=0 --group=0 --numeric-owner"

# How a target's artifact is packed. The same rule pack_windows_toolchain.sh
# follows, and for the same measured reason: the tar Windows ships is bsdtar
# linked against zlib alone, and hands every other codec to a program that is
# not there.
packing_for() {
    case "$1" in
    windows-*) printf 'tar.gz\n' ;;
    *) printf 'tar.zst\n' ;;
    esac
}

die() {
    printf 'pack_tool: %s\n' "$1" >&2
    exit 1
}

note() {
    printf '  %s\n' "$1" >&2
}

# On Windows a suffix is a permission: what says a file may be run is that it
# is called .exe. Asked once, from the target, rather than guessed at each use.
suffix_for_target() {
    case "$1" in
    windows-*) printf '.exe' ;;
    *) printf '' ;;
    esac
}

# --------------------------------------------------------------- the catalogue
#
# What each tool is, in the registry's vocabulary. A table and not a guess:
# `kind` is a closed vocabulary the registry validates against, and the rest is
# what a person reads in `pickup search`. These are the words the Linux
# artifacts were published with, repeated here so the two targets of one tool
# do not describe themselves differently.

role_of() {
    case "$1" in
    clang-format) printf 'formatter\n' ;;
    clang-tidy) printf 'linter\n' ;;
    clangd) printf 'language_server\n' ;;
    *) return 1 ;;
    esac
}

description_of() {
    case "$1" in
    clang-format) printf "LLVM's C and C++ formatter, the one molto fmt drives\n" ;;
    clang-tidy) printf "LLVM's C and C++ linter, the one molto lint drives\n" ;;
    clangd) printf 'The C and C++ language server an editor talks to, matching the compiler it indexes for\n' ;;
    *) return 1 ;;
    esac
}

homepage_of() {
    case "$1" in
    clang-format) printf 'https://clang.llvm.org/docs/ClangFormat.html\n' ;;
    clang-tidy) printf 'https://clang.llvm.org/extra/clang-tidy/\n' ;;
    clangd) printf 'https://clangd.llvm.org\n' ;;
    *) return 1 ;;
    esac
}

require_tools() {
    local packing="$1" tool
    for tool in tar file sha256sum; do
        command -v "$tool" >/dev/null || die "$tool is needed and not on the PATH"
    done
    case "$packing" in
    tar.gz) command -v gzip >/dev/null || die "gzip is needed and not on the PATH" ;;
    tar.zst) command -v zstd >/dev/null || die "zstd is needed and not on the PATH" ;;
    esac
}

# --------------------------------------------------------------- running

# Whether a program built for `target` can be started on this machine.
#
# Packing a Windows artifact used to mean packing it on Linux, where the only
# way to run one was wine. On a Windows machine every one of them is native, so
# what decides is the machine doing the packing and not the artifact.
needs_wine() {
    case "$1" in
    windows-*) ;;
    *) printf 'no\n'; return 0 ;;
    esac
    case "$(uname -s 2>/dev/null)" in
    MINGW* | MSYS* | CYGWIN*) printf 'no\n' ;;
    *) printf 'yes\n' ;;
    esac
}

require_wine() {
    [ -n "${WINE:-}" ] || die "WINE names the loader that verification runs through, and is unset"
    [ -x "$WINE" ] || die "WINE=$WINE is not executable"
    note "verifying through $("$WINE" --version 2>/dev/null || basename "$WINE")"
}

# Run the packed program. The directory it sits in is what a Windows loader
# searches first, which is the whole reason the libraries are staged beside it.
run_packed() {
    if [ "$NEEDS_WINE" = yes ]; then
        WINEDEBUG=-all "$WINE" "$@"
    else
        "$@"
    fi
}

# --------------------------------------------------------------- the libraries

# The libraries a program imports that this prefix is the source of.
#
# Read off the binary rather than listed here, because a list is a claim about
# a release: llvm-mingw moved clang-format from a static link to libclang-cpp
# between releases, and a hardcoded set would have packed an artifact that
# starts on the packer's machine and nowhere else.
#
# Only names the prefix actually holds are followed. What is left over is the
# operating system's own — KERNEL32, the api-ms-win-crt stubs — and shipping
# those would be both useless and wrong.
imports_of() {
    local reader="$1" binary="$2"
    "$reader" --coff-imports "$binary" 2>/dev/null |
        sed -n 's/^ *Name: \(.*\.dll\)$/\1/Ip' | sort -u
}

# Everything `binary` needs, transitively, that lives in this prefix's bin.
library_closure() {
    local reader="$1" bin="$2" binary="$3"
    local pending=("$binary") seen="" found="" name candidate

    while [ ${#pending[@]} -gt 0 ]; do
        local current="${pending[0]}"
        pending=("${pending[@]:1}")

        while IFS= read -r name; do
            [ -n "$name" ] || continue
            case " $seen " in *" $name "*) continue ;; esac
            candidate="$bin/$name"
            [ -f "$candidate" ] || continue
            seen="$seen $name"
            found="$found $candidate"
            pending+=("$candidate")
        done < <(imports_of "$reader" "$current")
    done

    printf '%s\n' $found
}

# The program that reads a PE's imports. Taken from the prefix being packed, so
# the answer comes from the same release as the binary it is about, and there is
# nothing to install on the packing machine.
find_reader() {
    local prefix="$1" suffix="$2"
    local reader="$prefix/bin/llvm-readobj$suffix"
    [ -x "$reader" ] || die "$reader is missing, and it is what reads which libraries a tool needs"
    printf '%s' "$reader"
}

# --------------------------------------------------------------- the recipe

write_recipe() {
    local out="$1" name="$2" version="$3" target="$4" binary="$5" packing="$6"

    {
        printf 'schema = 1\nform = "binary"\nkind = "tool"\n'
        printf 'name = "%s"\nversion = "%s"\ntarget = "%s"\n' "$name" "$version" "$target"
        # Declared rather than left to the registry to assume, the same way a
        # toolchain declares it: what a name ends in is not a statement.
        printf 'format = "%s"\n\n' "$packing"
        printf '[tool]\nkind = "%s"\nbinary = "%s"\n\n' "$(role_of "$name")" "$binary"
        printf '[about]\ndescription = "%s"\n' "$(description_of "$name")"
        printf 'license = "Apache-2.0 WITH LLVM-exception"\nhomepage = "%s"\n' "$(homepage_of "$name")"
    } > "$out"
}

# --------------------------------------------------------------- main

main() {
    [ $# -ge 4 ] || die "usage: pack_tool.sh <prefix> <name> <version> <target> [outdir]"
    local prefix="$1" name="$2" version="$3" target="$4" outdir="${5:-.}"
    [ -d "$prefix/bin" ] || die "$prefix has no bin/ directory"
    role_of "$name" >/dev/null || die "no role is defined for '$name', and guessing one would publish a lie"

    local packing suffix
    packing="$(packing_for "$target")"
    suffix="$(suffix_for_target "$target")"
    require_tools "$packing"

    NEEDS_WINE="$(needs_wine "$target")"
    [ "$NEEDS_WINE" = no ] || require_wine

    local source="$prefix/bin/$name$suffix"
    [ -f "$source" ] || die "$source is not there"

    local stage
    stage="$(mktemp -d)"
    # Expanded now, not when the trap fires: `stage` is local to main, and a
    # trap that runs after it has returned would read an unset name.
    trap "rm -rf '$stage'" EXIT
    mkdir -p "$stage/bin"

    note "packing $name $version for $target"
    # -p, and the same below: tar records what it is handed, so a copy carrying
    # today's date makes the archive differ from the one packed yesterday out of
    # the same release. Keeping the release's own timestamps is what makes the
    # sha256 a property of the input.
    cp -p "$source" "$stage/bin/"

    # The libraries, beside the program rather than under lib/: on Windows the
    # loader searches the directory the executable came from, and on Linux
    # there is nothing to copy because the binary is static.
    local library count=0
    for library in $(library_closure "$(find_reader "$prefix" "$suffix")" "$prefix/bin" "$source"); do
        cp -p "$library" "$stage/bin/"
        count=$((count + 1))
    done
    note "$count libraries travel with it, $(du -sh "$stage" | cut -f1) in total"

    # Proved before it is published, not after. A tool artifact that does not
    # start is one pickup will download, unpack and then refuse (install_service
    # calls this "the archive holds no binary that answers"), and finding that
    # out here costs one process.
    #
    # The whole output is searched rather than its first line: clang-format and
    # clangd open with their version, and clang-tidy opens with "LLVM
    # (http://llvm.org/):" and puts the number underneath. A check on line one
    # would reject a tool that answered perfectly well.
    local answer reported
    answer="$(run_packed "$stage/bin/$name$suffix" --version 2>/dev/null)" ||
        die "the packed $name does not run"
    [ -n "$answer" ] || die "the packed $name ran and said nothing"
    case "$answer" in
    *"$version"*) ;;
    *) die "the packed $name does not report $version" ;;
    esac
    reported="$(printf '%s\n' "$answer" | grep -m1 -F "$version" | sed 's/^ *//')"
    note "it answers: $reported"

    # The two directories tar also records. `cp -p` gave every file the date it
    # has in the release, but `.` and `./bin` were made just now, so without
    # this the archive still differs from the one packed a minute earlier out of
    # the same input.
    touch -r "$source" "$stage/bin" "$stage"

    mkdir -p "$outdir"
    local archive="$outdir/$name-$version-$target.$packing"
    case "$packing" in
    # -n so the blob has no build date in it: the same tree packed twice is the
    # same bytes, and the same sha256 the registry publishes.
    tar.gz) tar $REPRODUCIBLY -C "$stage" -c -I "gzip -$GZIP_LEVEL -n" -f "$archive" . ;;
    *) tar $REPRODUCIBLY -C "$stage" -c -I "zstd -$ZSTD_LEVEL -T0" -f "$archive" . ;;
    esac

    write_recipe "$outdir/recipe-$name-$target.toml" "$name" "$version" "$target" \
        "bin/$name$suffix" "$packing"

    printf '%s\n' "$archive"
    printf 'sha256: %s\n' "$(sha256sum "$archive" | cut -d' ' -f1)"
    printf 'bytes:  %s\n' "$(stat -c %s "$archive")"
    printf 'format: %s\n' "$packing"
    printf 'recipe: %s/recipe-%s-%s.toml\n' "$outdir" "$name" "$target"
}

main "$@"
