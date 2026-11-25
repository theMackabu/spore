#!/bin/dash
set -eu

version=0.172.0
asset=codex-package-aarch64-unknown-linux-musl.tar.gz
digest=faf17b8071927f33346a805dc8cb45de4e08c0d75cf0e5e68cecddca28b8861e
if [ "$#" -gt 1 ] || [ "$(uname -m)" != aarch64 ]; then
  echo "usage on AArch64: codex-install [release-archive]" >&2
  exit 2
fi

base="$HOME/.local/share/codex"
release="$base/$version"
bin="$HOME/.local/bin"
umask 077
mkdir -p -- "$base" "$bin"
stage=$(mktemp -d "$base/.install.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
trap 'exit 1' HUP INT TERM

if [ ! -x "$release/bin/codex" ]; then
  archive=${1:-$stage/$asset}
  if [ "$#" -eq 0 ]; then
    curl -fL --retry 3 -o "$archive" "https://github.com/openai/codex/releases/download/rust-v$version/$asset"
  fi
  actual=$(sha256sum < "$archive")
  if [ "${actual%% *}" != "$digest" ]; then
    echo "Codex $version archive failed SHA-256 verification" >&2
    exit 1
  fi
  mkdir "$stage/package"
  tar -xzf "$archive" -C "$stage/package"
  "$stage/package/bin/codex" --version
  mv -T -- "$stage/package" "$release" || [ -x "$release/bin/codex" ]
fi

link="$bin/codex"
if [ "$(readlink "$link" 2>/dev/null || true)" != "$release/bin/codex" ]; then
  ln -sT -- "$release/bin/codex" "$link" || [ "$(readlink "$link" 2>/dev/null)" = "$release/bin/codex" ]
fi
printf 'Installed %s\n' "$link"
