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
musl_cc=$(command -v aarch64-unknown-linux-musl-gcc)
musl_ar=$(command -v aarch64-unknown-linux-musl-ar)
musl_ranlib=$(command -v aarch64-unknown-linux-musl-ranlib)
src="$root/userland/third_party/zlib"
work="$build/zlib-build"
inst="$build/zlib-install"
stamp="$build/.zlib.stamp"
stamp_new="$build/.zlib.stamp.new"

mkdir -p "$build"
{
  git -C "$src" rev-parse HEAD
  cksum "$0"
  "$musl_cc" --version | sed -n '1p'
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
    -DCMAKE_C_COMPILER="$musl_cc" \
    -DCMAKE_AR="$musl_ar" \
    -DCMAKE_RANLIB="$musl_ranlib" \
    -DCMAKE_INSTALL_PREFIX="$inst" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DZLIB_BUILD_SHARED=OFF \
    -DZLIB_BUILD_STATIC=ON \
    -DZLIB_BUILD_TESTING=OFF \
    >/dev/null
)
cmake --build "$work" -j "$jobs" >/dev/null
cmake --install "$work" >/dev/null

printf 'zlib static musl build: %s\n' "$(git -C "$src" describe --tags --always)" >"$out"
mv "$stamp_new" "$stamp"
