#!/bin/bash
# Fetch Steam Audio 4.8.1 into this directory under the names Makefile.in
# expects: libphonon.so, libphonon32.so, LICENSE.steamaudio,
# THIRDPARTY.steamaudio.md.
#
# Upstream: https://github.com/ValveSoftware/steam-audio
# Release:  v4.8.1
# Asset:    steamaudio_4.8.1.zip
#   https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip
#
# PINNED_ZIP_SHA256 was computed on 2026-08-16 with sha256sum from the local
# copy /home/montys/Downloads/steamaudio_4.8.1(1).zip (official v4.8.1
# asset, 181171027 bytes). The four artifact hashes below were taken from
# that same zip after extraction: linux-x64/libphonon.so, linux-x86/libphonon.so
# (installed as libphonon32.so), THIRDPARTY.md as-is, and LICENSE.steamaudio
# derived from THIRDPARTY.md as described below.
#
# The binary zip has no standalone LICENSE file. THIRDPARTY.steamaudio.md is
# steamaudio/THIRDPARTY.md byte-for-byte (CRLF). LICENSE.steamaudio is a
# Proton packaging preamble plus the first Apache-2.0 banner in that file
# with CRLF stripped to LF, matching the existing contrib copies.
#
# Override the download with a pre-fetched zip:
#   STEAMAUDIO_ZIP=/path/to/steamaudio_4.8.1.zip ./contrib/fetch-steamaudio.sh

set -euo pipefail

PINNED_ZIP_SHA256='4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449'
PINNED_LIB64_SHA256='faf7c4d69e0f104366f6553cda38d3f946f761f7db223baf12663f6fbf2387da'
PINNED_LIB32_SHA256='1ce8790ee1e8126787118bf6df67eaac442ace3fa2e385344803bc9732d49570'
PINNED_LICENSE_SHA256='dcff25aeb2585c3e00575f9a36fbccf68482e1937d0bc25ee2d4c4a0ede18cb4'
PINNED_THIRDPARTY_SHA256='a95e143e7d2466e82a28d6680c7287d9fc7d772136c323ab538a6d24472e70f0'

STEAMAUDIO_URL='https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip'

CONTRIB="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

file_sha256() {
  sha256sum -- "$1" | awk '{print $1}'
}

die() {
  echo "fetch-steamaudio: $*" >&2
  exit 1
}

have_pinned_artifacts() {
  local path got expected
  local -A hashes=(
    [libphonon.so]="$PINNED_LIB64_SHA256"
    [libphonon32.so]="$PINNED_LIB32_SHA256"
    [LICENSE.steamaudio]="$PINNED_LICENSE_SHA256"
    [THIRDPARTY.steamaudio.md]="$PINNED_THIRDPARTY_SHA256"
  )
  for path in "${!hashes[@]}"; do
    expected="${hashes[$path]}"
    [[ -f "$CONTRIB/$path" ]] || return 1
    got="$(file_sha256 "$CONTRIB/$path")"
    [[ "$got" == "$expected" ]] || return 1
  done
  return 0
}

verify_sha256() {
  local path="$1" expected="$2" label="$3" got
  got="$(file_sha256 "$path")"
  if [[ "$got" != "$expected" ]]; then
    die "$label sha256 mismatch
  expected: $expected
  got:      $got
  file:     $path"
  fi
}

derive_license() {
  local thirdparty="$1" dest="$2"
  {
    cat <<'EOF'
This distribution includes the Steam Audio runtime library (libphonon.so,
version 4.8.1) in lib/x86_64-linux-gnu/ and lib/i386-linux-gnu/.

Steam Audio is Copyright Valve Corporation and is redistributed here in
unmodified binary form under the Apache License, Version 2.0, reproduced in
full below. Upstream: https://github.com/ValveSoftware/steam-audio

Third-party components bundled inside Steam Audio itself are listed in
THIRDPARTY.STEAMAUDIO.md alongside this file.


EOF
    tr -d '\r' < "$thirdparty" | awk '
      /^                                 Apache License$/ { p=1 }
      p { print }
      /^   limitations under the License\.$/ && p { exit }
    '
  } > "$dest"
}

if have_pinned_artifacts; then
  echo "fetch-steamaudio: Steam Audio 4.8.1 already present, nothing to do"
  exit 0
fi

command -v unzip >/dev/null || die "unzip is required"
command -v sha256sum >/dev/null || die "sha256sum is required"

workdir="$(mktemp -d "${TMPDIR:-/tmp}/fetch-steamaudio.XXXXXX")"
cleanup() {
  rm -rf -- "$workdir"
}
trap cleanup EXIT

if [[ -n "${STEAMAUDIO_ZIP:-}" ]]; then
  [[ -f "$STEAMAUDIO_ZIP" ]] || die "STEAMAUDIO_ZIP is not a file: $STEAMAUDIO_ZIP"
  zipfile="$STEAMAUDIO_ZIP"
else
  command -v curl >/dev/null || die "curl is required to download Steam Audio"
  zipfile="$workdir/steamaudio_4.8.1.zip"
  echo "fetch-steamaudio: downloading $STEAMAUDIO_URL"
  curl -fL --retry 3 --retry-delay 2 -o "$zipfile" -- "$STEAMAUDIO_URL"
fi

verify_sha256 "$zipfile" "$PINNED_ZIP_SHA256" "steamaudio_4.8.1.zip"

extract="$workdir/extract"
staging="$workdir/staging"
mkdir -p -- "$extract" "$staging"

unzip -q -o -d "$extract" -- "$zipfile" \
  steamaudio/lib/linux-x64/libphonon.so \
  steamaudio/lib/linux-x86/libphonon.so \
  steamaudio/THIRDPARTY.md

cp -a -- "$extract/steamaudio/lib/linux-x64/libphonon.so" "$staging/libphonon.so"
cp -a -- "$extract/steamaudio/lib/linux-x86/libphonon.so" "$staging/libphonon32.so"
cp -a -- "$extract/steamaudio/THIRDPARTY.md" "$staging/THIRDPARTY.steamaudio.md"
derive_license "$extract/steamaudio/THIRDPARTY.md" "$staging/LICENSE.steamaudio"

verify_sha256 "$staging/libphonon.so" "$PINNED_LIB64_SHA256" "libphonon.so"
verify_sha256 "$staging/libphonon32.so" "$PINNED_LIB32_SHA256" "libphonon32.so"
verify_sha256 "$staging/LICENSE.steamaudio" "$PINNED_LICENSE_SHA256" "LICENSE.steamaudio"
verify_sha256 "$staging/THIRDPARTY.steamaudio.md" "$PINNED_THIRDPARTY_SHA256" "THIRDPARTY.steamaudio.md"

mkdir -p -- "$CONTRIB"
mv -f -- "$staging/libphonon.so" "$CONTRIB/libphonon.so"
mv -f -- "$staging/libphonon32.so" "$CONTRIB/libphonon32.so"
mv -f -- "$staging/LICENSE.steamaudio" "$CONTRIB/LICENSE.steamaudio"
mv -f -- "$staging/THIRDPARTY.steamaudio.md" "$CONTRIB/THIRDPARTY.steamaudio.md"

echo "fetch-steamaudio: installed Steam Audio 4.8.1 into $CONTRIB"
