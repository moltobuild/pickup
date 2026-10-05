#!/bin/sh
#
# Install the molto release named in .github/molto-version onto PATH, after
# checking it is the file the release hashed.
#
#   install-molto.sh <asset suffix> <sha256 command>
#
# The suffix is the part of the asset name after the version (`x86_64-linux`,
# `arm64-macos`, `x86_64-windows.exe`); the hash command is passed in because
# macOS has no sha256sum. One file names the version for every workflow:
# pickup is built and tested by a published molto, never by molto's tip, and a
# toolchain manager whose CI drifted with another repository could not tell its
# bugs from that repository's.
set -eu

suffix=$1
sha=$2
here=$(cd "$(dirname "$0")" && pwd)
version=$(tr -d ' \r\n' < "$here/molto-version")
releases=https://github.com/moltobuild/molto/releases/download
asset="molto-${version}-${suffix}"
dir="${RUNNER_TEMP:-/tmp}/molto-bin"

mkdir -p "$dir"
cd "$dir"
curl -fsSLO --retry 3 "${releases}/v${version}/${asset}"
curl -fsSLO --retry 3 "${releases}/v${version}/SHA256SUMS"
$sha --check --ignore-missing SHA256SUMS

case "$suffix" in
    *.exe) name=molto.exe ;;
    *) name=molto ;;
esac
mv "$asset" "$name"
chmod +x "$name"

# Under MSYS2 the shell does not inherit the runner's PATH, so GITHUB_PATH
# would not reach it; its own /usr/local/bin is on every MSYS2 shell's PATH.
if command -v cygpath >/dev/null 2>&1; then
    mkdir -p /usr/local/bin
    mv "$name" /usr/local/bin/
    molto --version
else
    echo "$dir" >> "$GITHUB_PATH"
    "./$name" --version
fi
