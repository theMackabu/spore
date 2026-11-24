#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: build.sh SOURCE_ROOT BUILD_DIR PROJECT_BUILD_ROOT OUTPUT" >&2
  exit 2
fi

root=$1
build=$2
project_build=$3
out=$4
case "$out" in
  /*) ;;
  *) out="$PWD/$out" ;;
esac

musl_cc=$(command -v aarch64-unknown-linux-musl-gcc)
musl_strip=$(command -v aarch64-unknown-linux-musl-strip)
apk_src="$root/userland/third_party/apk-tools"
apk_work="$build/apk-tools-src"
sdk="$project_build/userland/prebuilt/apk-sdk"
stamp="$build/.apk.stamp"
stamp_new="$build/.apk.stamp.new"

mkdir -p "$build"
{
  git -C "$apk_src" rev-parse HEAD
  cat "$sdk.stamp"
  cksum "$0"
  cksum "$root"/userland/usr/local/bin/apk/patches/*.patch
  "$musl_cc" --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$out" ] && [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$apk_work"
mkdir -p "$apk_work"
git -C "$apk_src" archive --format=tar HEAD | tar -x -C "$apk_work"

for patch_file in "$root"/userland/usr/local/bin/apk/patches/*.patch; do
  [ -e "$patch_file" ] || continue
  (cd "$apk_work" && patch -p1 < "$patch_file" >/dev/null)
done

export PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1 PKG_CONFIG_ALLOW_SYSTEM_LIBS=1
export PKG_CONFIG_LIBDIR="$sdk/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$sdk" PKG_CONFIG_PATH=
export CFLAGS_ALL="-std=gnu11 -O2 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -I\"$sdk/usr/include\""
export LDFLAGS_ALL="-L\"$sdk/usr/lib\""
make -C "$apk_work" -f Make.rules -j"${NUM_JOBS:-4}" build=src src/apk.static \
  CC="$musl_cc" AR=aarch64-unknown-linux-musl-ar PKG_CONFIG=pkg-config \
  STATIC=y CRYPTO=mbedtls URL_BACKEND=wget ZSTD=no LUA=no \
  SED=sed LN='ln -sf' VERSION="$(cat "$apk_work/VERSION")" SO_VERSION="$(cat "$apk_work/SOVERSION")" \
  LIBS_apk.static='-ldl'
"$musl_strip" -o "$out" "$apk_work/src/apk.static"
mv "$stamp_new" "$stamp"
