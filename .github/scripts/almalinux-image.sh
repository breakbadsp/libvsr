#!/usr/bin/env bash
# Stdout is the image name; build diagnostics go to stderr.
set -euo pipefail
cd "$(dirname "$0")"
version=${ALMA_VERSION:?set ALMA_VERSION}
case $version in 8|9) ;; *) echo 'ALMA_VERSION must be 8 or 9.' >&2; exit 2 ;; esac
digest=$(cat almalinux-ci.Dockerfile almalinux-toolchain.sh | sha256sum)
image="localhost/libvsr-ci:alma$version-${digest:0:20}"
lock_dir="${XDG_CACHE_HOME:-$HOME/.cache}/libvsr"
mkdir -p "$lock_dir"
exec 9> "$lock_dir/alma$version-image.lock"
flock 9
if docker image inspect "$image" > /dev/null 2>&1; then
  echo "Reuse prepared image: $image" >&2
else
  docker build --build-arg "ALMA_VERSION=$version" \
    -f almalinux-ci.Dockerfile -t "$image" . >&2
fi
printf '%s\n' "$image"
