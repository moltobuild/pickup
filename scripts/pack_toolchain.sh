#!/usr/bin/env bash
#
# Pack an installed toolchain prefix into the artifact the registry serves and
# pickup installs: one tar.zst holding the tree exactly as it should be
# unpacked, plus the recipe.toml that describes it.
#
# What this script is really about is what it leaves out. A full LLVM release is
# 735 MB on disk and a conda gcc is 856 MB, of which a C and C++ build uses a
# fraction: the rest is locale data, sanitizers nobody linked, an Objective-C
# backend, and debugging tools that belong in their own `tool` artifact. The
# exclusion lists below are the point of the file; each one is justified where
# it is made, because an exclusion that turns out to be wrong is a compiler that
# fails on someone else's machine.
#
# Nothing is selected at install time: pickup unpacks the whole archive and
# adopts it (spec.md, "What gets installed"). The decision about what to keep is
# taken here, once, by whoever publishes.
#
# Usage:
#   scripts/pack_toolchain.sh <prefix> <name> <version> <target> [outdir]
#
#   prefix   an installed toolchain, the directory holding bin/
#   name     the registry name: gcc, clang
#   version  the version the compiler reports, exactly
#   target   the platform literal pickup asks for: linux-x86_64
#   outdir   where the archive and the recipe are written (default: .)
#
# Environment:
#   PACK_SYSROOT   a sysroot to ship instead of the one in the prefix. What
#                  this really chooses is the oldest glibc the toolchain's
#                  output will run on, so it is the difference between a
#                  compiler that produces portable binaries and one that
#                  produces binaries for the packer's own machine.

set -euo pipefail

readonly ZSTD_LEVEL=19
readonly C_STANDARDS="c89 c99 c11 c17 c2x c23 c2y"
readonly CXX_STANDARDS="c++11 c++14 c++17 c++20 c++23 c++26"

die() {
    printf 'pack_toolchain: %s\n' "$1" >&2
    exit 1
}

note() {
    printf '  %s\n' "$1" >&2
}

require_tools() {
    for tool in tar zstd strip file sha256sum; do
        command -v "$tool" >/dev/null || die "$tool is needed and not on the PATH"
    done
    tar --help 2>/dev/null | grep -q zstd || die "this tar cannot open zstd archives"
}

# The family decides the whole layout, and it is read off the tree rather than
# asked for: a prefix that disagrees with what the caller typed would produce an
# archive that installs and then fails to compile.
detect_family() {
    local prefix="$1"
    if [ -d "$prefix/lib/clang" ]; then
        printf 'clang'
    elif [ -d "$prefix/libexec/gcc" ]; then
        printf 'gcc'
    else
        die "$prefix looks like neither a clang nor a gcc installation"
    fi
}

# The triple the compiler answers with, which names the directories inside the
# prefix and goes into the recipe. Not the same thing as `target`: that one is
# the platform literal pickup matches on (registry_source.c).
detect_triple() {
    local driver="$1"
    "$driver" -dumpmachine
}

# ---------------------------------------------------------------- clang

stage_clang() {
    local prefix="$1" stage="$2" triple="$3"
    local version_dir
    version_dir="$(basename "$(find "$prefix/lib/clang" -maxdepth 1 -mindepth 1 -type d | head -1)")"

    mkdir -p "$stage/bin" "$stage/lib/clang/$version_dir/lib"

    # The driver and its aliases. clang, clang++ and clang-cpp are symlinks to
    # one 225 MB binary; copying them as symlinks is the difference between one
    # copy and four.
    local driver
    driver="$(find "$prefix/bin" -maxdepth 1 -name 'clang-[0-9]*' | head -1)"
    [ -n "$driver" ] || die "no clang-N driver under $prefix/bin"
    cp -a "$driver" "$stage/bin/"
    for alias in clang clang++ clang-cpp; do
        [ -e "$prefix/bin/$alias" ] && cp -a "$prefix/bin/$alias" "$stage/bin/" || true
    done

    # Left out of the archive on purpose:
    #   clang-format, clang-tidy  they are `tool` artifacts of their own, which
    #                             the registry already publishes separately
    #   lld                       191 MB for a linker clang does not use unless
    #                             asked; the system linker does this job
    #   llvm-*                    a development kit, not a build requirement

    # The C++ standard library the driver brings, and libunwind with it.
    [ -d "$prefix/lib/$triple" ] && cp -a "$prefix/lib/$triple" "$stage/lib/" || true
    # Its headers, plus whatever else the prefix publishes as includes.
    [ -d "$prefix/include" ] && cp -a "$prefix/include" "$stage/include" || true

    # The builtin headers: stddef.h and friends. Without these the compiler
    # cannot parse its own idea of C.
    cp -a "$prefix/lib/clang/$version_dir/include" "$stage/lib/clang/$version_dir/include"

    # compiler-rt, trimmed. builtins and the crt objects are what every link
    # needs; asan, ubsan and lsan are kept because they are what a debug build
    # reaches for. tsan, msan, dfsan, hwasan, memprof, fuzzer, xray and cfi come
    # to 60 MB and are opt-in tools for work nobody does by accident.
    local rt_src="$prefix/lib/clang/$version_dir/lib/$triple"
    if [ -d "$rt_src" ]; then
        local rt_dst="$stage/lib/clang/$version_dir/lib/$triple"
        mkdir -p "$rt_dst"
        find "$rt_src" -maxdepth 1 \
            \( -name 'clang_rt.crt*.o' \
            -o -name 'libclang_rt.builtins*' \
            -o -name 'libclang_rt.asan*' \
            -o -name 'libclang_rt.ubsan*' \
            -o -name 'libclang_rt.lsan.a' \) \
            -exec cp -a {} "$rt_dst/" \;
    fi
}

# ---------------------------------------------------------------- gcc

stage_gcc() {
    local prefix="$1" stage="$2" triple="$3" version="$4"

    mkdir -p "$stage/bin" "$stage/lib" "$stage/libexec/gcc/$triple/$version"

    # The drivers, under both the short names and the prefixed ones. pickup
    # looks for bin/gcc first (install_service.c), and the short names are
    # symlinks to the prefixed binaries, so both have to travel.
    local short="gcc g++ cpp cc c++ gcc-ar gcc-nm gcc-ranlib"
    local prefixed="gcc g++ cpp c++ gcc-ar gcc-nm gcc-ranlib as ld ld.bfd ar nm \
        ranlib strip objcopy objdump readelf c++filt elfedit addr2line size strings"
    local name
    for name in $short; do
        [ -e "$prefix/bin/$name" ] && cp -a "$prefix/bin/$name" "$stage/bin/" || true
    done
    for name in $prefixed; do
        [ -e "$prefix/bin/$triple-$name" ] && cp -a "$prefix/bin/$triple-$name" "$stage/bin/" || true
    done

    # The compiler proper. cc1 and cc1plus are the C and C++ front ends; without
    # collect2 and lto-wrapper the driver cannot link at all.
    #   cc1obj, cc1objplus  72 MB of Objective-C, a language molto does not build
    #   lto1                34 MB, and RFC-0003 has `lto` reserved, not read
    local backend="$prefix/libexec/gcc/$triple/$version"
    for name in cc1 cc1plus collect2 lto-wrapper liblto_plugin.so; do
        [ -e "$backend/$name" ] && cp -a "$backend/$name" "$stage/libexec/gcc/$triple/$version/" || true
    done

    # libgcc, libstdc++, the C++ headers and the crt objects the driver links.
    cp -a "$prefix/lib/gcc" "$stage/lib/gcc"
    #   plugin/  13 MB of GCC plugin headers, a compiler-development kit
    rm -rf "$stage/lib/gcc/$triple/$version/plugin"
    #   the standalone sanitizer archives, matching the clang trim above
    local unwanted
    for unwanted in libtsan libmsan libhwasan; do
        rm -f "$stage/lib/gcc/$triple/$version/$unwanted".*
    done
    for name in libstdc++.so* libgcc_s.so* libatomic.so* libgomp.so* libasan.so* libubsan.so*; do
        cp -a "$prefix/lib/"$name "$stage/lib/" 2>/dev/null || true
    done

    # The binutils the driver invokes through its own triple directory, and
    # beside them whatever the build put under the triple rather than under
    # lib/gcc: newer conda toolchains keep the C++ headers and libstdc++ there,
    # older ones keep them in lib/gcc. Both places travel, and the one that is
    # absent costs nothing.
    mkdir -p "$stage/$triple"
    local under
    for under in bin lib include; do
        [ -d "$prefix/$triple/$under" ] && cp -a "$prefix/$triple/$under" "$stage/$triple/$under" || true
    done

    stage_gcc_sysroot "${PACK_SYSROOT:-$prefix/$triple/sysroot}" "$stage/$triple/sysroot"
}

# A conda gcc cannot compile against the host's libc: it is configured with its
# own sysroot and `--sysroot=/` fails on the first #include. So the sysroot
# travels — but only the part a compiler reads. 439 MB of it is compiled locale
# data and 8 MB is gconv, which belong to a running program, not to a build.
stage_gcc_sysroot() {
    local src="$1" dst="$2"
    [ -d "$src" ] || return 0

    mkdir -p "$dst/usr"
    # The C headers, and the kernel's beside them: <errno.h> includes
    # <linux/errno.h>, so a sysroot without them compiles C and then fails on
    # the first C++ header that reaches for errno.
    cp -a "$src/usr/include" "$dst/usr/include"

    # A sysroot keeps its libraries in lib64, usr/lib64, or both, depending on
    # which build it came from. Whichever exist travel, minus what only a
    # running program uses.
    local libdir
    for libdir in lib64 usr/lib64; do
        copy_sysroot_libdir "$src/$libdir" "$dst/$libdir"
    done

    #   share/  locale data and man pages, for a machine nobody runs programs on
    #   sbin, var, etc  a root filesystem's furniture; a compiler reads none of it
    rm -rf "$dst/usr/share" "$dst/usr/sbin" "$dst/var" "$dst/etc" "$dst/sbin"

    # lib, usr/lib and usr/lib64 are symlinks into lib64 in some sysroots. The
    # linker looks for crt1.o down every one of those paths, so a missing
    # symlink reads as a missing C runtime.
    local link
    for link in lib usr/lib usr/lib64; do
        if [ -L "$src/$link" ]; then
            rm -rf "${dst:?}/$link"
            cp -a "$src/$link" "$dst/$link"
        fi
    done
}

# One library directory of a sysroot, minus the three things in it that a
# compiler never reads: `locale` is compiled locale data and is by far the
# largest thing in a glibc tree (102 MB in the 2.17 sysroot, 439 MB in the
# 2.39 one), `gconv` is character-set conversion, and `audit` is a loader
# plugin directory. All three belong to a program at run time, on a machine
# that is not this one.
copy_sysroot_libdir() {
    local src="$1" dst="$2"
    [ -d "$src" ] || return 0
    mkdir -p "$dst"

    local entry
    for entry in "$src"/*; do
        case "$(basename "$entry")" in
            locale | gconv | audit) continue ;;
            *) cp -a "$entry" "$dst/" ;;
        esac
    done
}

# ---------------------------------------------------------------- shared

# Configuration files are written by pickup when it adopts a toolchain, against
# the paths of the machine it is installing on. Shipping one embeds the packer's
# filesystem in every install — which is how a --sysroot pointing at a staging
# directory that no longer exists ended up on this machine.
drop_configuration() {
    find "$1" -name '*.cfg' -delete
}

strip_binaries() {
    local stage="$1"
    local roots="$stage/bin"
    [ -d "$stage/libexec" ] && roots="$roots $stage/libexec" || true
    find $roots -type f | while read -r binary; do
        if file -b "$binary" | grep -q 'ELF.*executable'; then
            strip --strip-unneeded "$binary" 2>/dev/null || true
        fi
    done
}

# Compile, link and *run* a program in both languages. Counting features is not
# enough and pickup says so: its probes avoid headers on purpose, so a compiler
# with no working sysroot passes every one of them and then fails on <stdio.h>.
# An archive that cannot do this would install and then break a real build.
verify_stage() {
    local stage="$1" family="$2" triple="$3"
    local work
    work="$(mktemp -d)"

    printf '#include <stdio.h>\nint main(void){puts("ok");return 0;}\n' > "$work/probe.c"
    printf '#include <iostream>\nint main(){std::cout<<"ok\\n";return 0;}\n' > "$work/probe.cpp"

    local cc cxx rpath
    if [ "$family" = clang ]; then
        cc="$stage/bin/clang"
        cxx="$stage/bin/clang++"
        rpath="$stage/lib/$triple"
    else
        cc="$stage/bin/gcc"
        cxx="$stage/bin/g++"
        rpath="$stage/lib"
    fi

    "$cc" "$work/probe.c" -o "$work/probe_c" || die "the trimmed toolchain cannot compile C"
    "$work/probe_c" > /dev/null || die "the program the trimmed toolchain built does not run"
    note "C compiles, links and runs"

    # The C++ driver has to work as well as exist: pickup fails the whole
    # install when a toolchain carries a C++ driver that cannot build a program
    # (install_service.c). libc++ needs both the flag and the rpath; libstdc++
    # needs only the rpath, and neither is guessed here — the same two attempts
    # pickup makes when it adopts the toolchain.
    if "$cxx" -stdlib=libc++ -Wl,-rpath,"$rpath" "$work/probe.cpp" -o "$work/probe_x" 2>/dev/null \
        && "$work/probe_x" > /dev/null 2>&1; then
        note "C++ compiles, links and runs against the bundled libc++"
    elif "$cxx" -Wl,-rpath,"$rpath" "$work/probe.cpp" -o "$work/probe_x" 2>/dev/null \
        && "$work/probe_x" > /dev/null 2>&1; then
        note "C++ compiles, links and runs against the bundled libstdc++"
    else
        rm -rf "$work"
        die "the trimmed toolchain carries a C++ driver that cannot build a program"
    fi
    rm -rf "$work"
}

# The C library a program will be linked against, read out of the archive.
glibc_of_sysroot() {
    local libc="$1/lib64/libc.so.6"
    [ -e "$libc" ] || return 0
    # "stable release version 2.17, by Roland McGrath" in one release and
    # "release version 2.39." in another, so the number is taken up to whatever
    # punctuation follows it.
    strings -a "$libc" 2>/dev/null \
        | sed -n 's/.*release version \([0-9]\+\.[0-9]\+\).*/\1/p' \
        | head -1
}

# Refuse a toolchain whose C library is newer than the host's.
#
# This one is not theoretical. A gcc packed with conda's glibc 2.39 sysroot
# compiles, links and runs `puts("ok")` perfectly on a 2.35 machine — and then
# produces a real program that dies at startup with "GLIBC_2.38 not found",
# because the link resolved symbols against 2.39 while the loader offers 2.35.
# The probe misses it precisely because a small program uses no symbol young
# enough to notice, so the sysroot is compared directly instead.
#
# The rule is one-directional: a sysroot *older* than the host is the whole
# point of shipping one, since it is what makes the output run on machines
# older than the packer's.
verify_sysroot_age() {
    local stage="$1" triple="$2"
    local shipped host
    shipped="$(glibc_of_sysroot "$stage/$triple/sysroot")"
    [ -n "$shipped" ] || return 0
    host="$(ldd --version | sed -n '1s/.*[^0-9]\([0-9]\+\.[0-9]\+\).*/\1/p')"

    if [ "$(printf '%s\n%s\n' "$shipped" "$host" | sort -V | tail -1)" = "$shipped" ] \
        && [ "$shipped" != "$host" ]; then
        die "the sysroot ships glibc $shipped and this host runs $host, so this \
toolchain would build programs that do not start here. Pack it with an older \
sysroot: PACK_SYSROOT=<dir> names one"
    fi
    note "sysroot carries glibc $shipped, against a host running $host"
}

# Which -std values the compiler actually accepts, asked rather than assumed:
# a table written by hand goes stale the first time a version moves.
accepted_standards() {
    local driver="$1" language="$2" candidates="$3"
    local work accepted=""
    work="$(mktemp -d)"
    printf 'int main(void){return 0;}\n' > "$work/probe.c"

    local standard separator=""
    for standard in $candidates; do
        if "$driver" -x "$language" "-std=$standard" -fsyntax-only "$work/probe.c" 2>/dev/null; then
            accepted="$accepted$separator\"$standard\""
            separator=", "
        fi
    done
    rm -rf "$work"
    printf '%s' "$accepted"
}

# The capability vocabulary pickup resolves against, taken from pickup itself
# rather than written out here: it is pickup that decides what these names mean
# and it is pickup that will read them back.
#
# The staged tree is put at the front of PATH so that pickup discovers it the
# way it discovers any compiler, and a throwaway PICKUP_HOME keeps the probe
# cache of the machine doing the packing out of it. Packing a prefix that is
# not installed anywhere is the normal case -- a toolchain assembled from
# upstream packages has never been through `pickup install`.
detected_features() {
    local stage="$1"
    local home id
    home="$(mktemp -d)"

    id="$(PICKUP_HOME="$home" PATH="$stage/bin:$PATH" pickup list --format toml 2>/dev/null \
        | awk -v p="$stage/" '$0 ~ /^id = / {id=$3} $0 ~ /^path = / && index($0, p) {print id; exit}' \
        | tr -d '"')"
    if [ -n "$id" ]; then
        PICKUP_HOME="$home" PATH="$stage/bin:$PATH" pickup show "$id" --format toml 2>/dev/null \
            | awk -F' = ' '/^c_features|^cxx_features/ {gsub(/[][]/, "", $2); print $2}' \
            | paste -sd, - | sed 's/, */, /g'
    fi
    rm -rf "$home"
}

write_recipe() {
    local out="$1" name="$2" version="$3" target="$4" family="$5" triple="$6"
    local stage="$7"
    local c_driver cxx_driver stdlib runtime
    if [ "$family" = clang ]; then
        c_driver="bin/clang"; cxx_driver="bin/clang++"
        stdlib="libc++"; runtime="\"lib/$triple\""
    else
        c_driver="bin/gcc"; cxx_driver="bin/g++"
        stdlib="libstdc++"; runtime="\"lib\""
    fi

    {
        printf 'schema = 1\nform = "binary"\nkind = "toolchain"\n'
        printf 'name = "%s"\nversion = "%s"\ntarget = "%s"\n\n' "$name" "$version" "$target"
        printf 'provides = [%s]\n\n' "$(detected_features "$stage")"
        printf '[toolchain]\nvendor = "%s"\ntriple = "%s"\n' "$family" "$triple"
        printf 'c_driver = "%s"\ncxx_driver = "%s"\n\n' "$c_driver" "$cxx_driver"
        printf '[toolchain.c]\nstd = [%s]\ncompile_flags = []\nlink_flags = []\nruntime_dirs = []\n\n' \
            "$(accepted_standards "$stage/$c_driver" c "$C_STANDARDS")"
        printf '[toolchain.cxx]\nstd = [%s]\nstdlib = "%s"\n' \
            "$(accepted_standards "$stage/$cxx_driver" c++ "$CXX_STANDARDS")" "$stdlib"
        printf 'compile_flags = []\nlink_flags = []\nruntime_dirs = [%s]\n\n' "$runtime"
        printf '[about]\ndescription = "%s %s for %s, trimmed to what a C and C++ build uses"\n' \
            "$name" "$version" "$target"
    } > "$out"
}

main() {
    [ $# -ge 4 ] || die "usage: pack_toolchain.sh <prefix> <name> <version> <target> [outdir]"
    local prefix="$1" name="$2" version="$3" target="$4" outdir="${5:-.}"
    [ -d "$prefix/bin" ] || die "$prefix has no bin/ directory"
    require_tools
    mkdir -p "$outdir"

    local family triple stage archive
    family="$(detect_family "$prefix")"
    stage="$(mktemp -d)"
    # Expanded now, not on exit: by the time the trap runs, main's locals are
    # gone and `set -u` would turn the cleanup into an error of its own.
    trap "rm -rf '$stage'" EXIT

    note "packing $name $version ($family) from $prefix"
    if [ "$family" = clang ]; then
        triple="$(detect_triple "$prefix/bin/clang")"
        stage_clang "$prefix" "$stage" "$triple"
    else
        triple="$(detect_triple "$prefix/bin/gcc")"
        stage_gcc "$prefix" "$stage" "$triple" "$version"
    fi

    drop_configuration "$stage"
    strip_binaries "$stage"
    note "trimmed to $(du -sh "$stage" | cut -f1), from $(du -sh "$prefix" | cut -f1)"
    verify_sysroot_age "$stage" "$triple"
    verify_stage "$stage" "$family" "$triple"

    # bin/ has to sit at the root: pickup extracts with --strip-components=0, so
    # one extra directory level is a toolchain it cannot find.
    archive="$outdir/$name-$version-$target.tar.zst"
    tar -C "$stage" -c -I "zstd -$ZSTD_LEVEL -T0" -f "$archive" .
    write_recipe "$outdir/recipe.toml" "$name" "$version" "$target" "$family" "$triple" \
        "$stage"

    printf '%s\n' "$archive"
    printf 'sha256: %s\n' "$(sha256sum "$archive" | cut -d' ' -f1)"
    printf 'bytes:  %s\n' "$(stat -c %s "$archive")"
    printf 'recipe: %s/recipe.toml\n' "$outdir"
}

main "$@"
