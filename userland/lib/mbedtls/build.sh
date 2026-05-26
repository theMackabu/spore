#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: build.sh SOURCE_ROOT BUILD_DIR OUTPUT" >&2
  exit 2
fi

root=$1
build=$2
out=$3

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
src="$root/userland/third_party/mbedtls"
work="$build/mbedtls-build"
inst="$build/mbedtls-install"
stamp="$build/.mbedtls.stamp"
stamp_new="$build/.mbedtls.stamp.new"

mkdir -p "$build"
{
  git -C "$src" rev-parse HEAD
  cksum "$0"
  aarch64-unknown-linux-musl-gcc --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$out" ] && [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$work" "$inst"
mkdir -p "$work" "$inst"

(
  cd "$work"
  cmake "$src" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-unknown-linux-musl-gcc \
    -DCMAKE_AR=/run/current-system/sw/bin/aarch64-unknown-linux-musl-ar \
    -DCMAKE_RANLIB=/run/current-system/sw/bin/aarch64-unknown-linux-musl-ranlib \
    -DCMAKE_INSTALL_PREFIX="$inst" \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DLINK_WITH_PTHREAD=OFF \
    -DDISABLE_PACKAGE_CONFIG_AND_INSTALL=OFF \
    >/dev/null
)
cmake --build "$work" -j "$jobs" >/dev/null
cmake --install "$work" >/dev/null

printf 'mbedTLS static musl build: %s\n' "$(git -C "$src" describe --tags --always)" >"$out"
mv "$stamp_new" "$stamp"
