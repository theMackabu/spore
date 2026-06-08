#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: build.sh SOURCE_ROOT BUILD_DIR OUTPUT" >&2
  exit 2
fi

root=$1
build=$2
out=$3

# Static musl TLS backend for curl. Build for handshake/runtime speed rather than
# minimum size; HTTPS was visibly CPU-bound with MinSizeRel crypto.

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
musl_cc=$(command -v aarch64-unknown-linux-musl-gcc)
musl_ar=$(command -v aarch64-unknown-linux-musl-ar)
musl_ranlib=$(command -v aarch64-unknown-linux-musl-ranlib)
src="$root/userland/third_party/mbedtls"
work="$build/mbedtls-build"
inst="$build/mbedtls-install"
stamp="$build/.mbedtls.stamp"
stamp_new="$build/.mbedtls.stamp.new"

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
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=armv8-a+crypto -DMBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_ONLY" \
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
