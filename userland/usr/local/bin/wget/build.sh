#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: build.sh SOURCE_ROOT BUILD_DIR PROJECT_BUILD_ROOT OUTPUT" >&2
  exit 2
fi

root=$1
build=$2
_project_build=$3
out=$4
case "$out" in
  /*) ;;
  *) out="$PWD/$out" ;;
esac

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
busybox_src="$root/userland/third_party/busybox"
busybox_build="$build/busybox-build"
stamp="$build/.wget.stamp"
stamp_new="$build/.wget.stamp.new"

mkdir -p "$build"
{
  git -C "$busybox_src" rev-parse HEAD
  cksum "$0"
  aarch64-unknown-linux-musl-gcc --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$out" ] && [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$busybox_build"
mkdir -p "$busybox_build"
make -C "$busybox_src" O="$busybox_build" allnoconfig >/dev/null

cfg="$busybox_build/.config"
enable() {
  sym=$1
  if grep -q "^# CONFIG_${sym} is not set$" "$cfg"; then
    sed -i.bak "s/^# CONFIG_${sym} is not set$/CONFIG_${sym}=y/" "$cfg"
  elif grep -q "^CONFIG_${sym}=" "$cfg"; then
    sed -i.bak "s/^CONFIG_${sym}=.*/CONFIG_${sym}=y/" "$cfg"
  else
    printf 'CONFIG_%s=y\n' "$sym" >>"$cfg"
  fi
  rm -f "$cfg.bak"
}

enable STATIC
enable SHOW_USAGE
enable FEATURE_VERBOSE_USAGE
enable LONG_OPTS
enable WGET
enable FEATURE_WGET_LONG_OPTIONS
enable FEATURE_WGET_AUTHENTICATION
enable FEATURE_WGET_TIMEOUT
enable FEATURE_WGET_HTTPS
enable TLS

sed -i.bak 's/^CONFIG_CROSS_COMPILER_PREFIX=.*/CONFIG_CROSS_COMPILER_PREFIX="aarch64-unknown-linux-musl-"/' "$cfg"
rm -f "$cfg.bak"
yes '' | make -C "$busybox_src" O="$busybox_build" oldconfig >/dev/null
make -C "$busybox_src" O="$busybox_build" -j"$jobs" busybox >/dev/null
aarch64-unknown-linux-musl-strip -o "$out" "$busybox_build/busybox"
mv "$stamp_new" "$stamp"
