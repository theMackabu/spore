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
musl_ar=$(command -v aarch64-unknown-linux-musl-ar)
musl_strip=$(command -v aarch64-unknown-linux-musl-strip)
apk_src="$root/userland/third_party/apk-tools"
apk_work="$build/apk-tools-src"
apk_build="$build/apk-tools-build"
sdk="$project_build/userland/prebuilt/apk-sdk"
cross="$build/apk-cross.ini"
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

rm -rf "$apk_work" "$apk_build"
mkdir -p "$apk_work" "$apk_build"
git -C "$apk_src" archive --format=tar HEAD | tar -x -C "$apk_work"
# Spore does not implement Linux O_TMPFILE yet. Force apk-tools onto its
# named-temporary-file fallback in the build copy.
sed -i.bak 's|#define HAVE_O_TMPFILE|/* #undef HAVE_O_TMPFILE */|g' "$apk_work/src/io.c"

for patch_file in "$root"/userland/usr/local/bin/apk/patches/*.patch; do
  [ -e "$patch_file" ] || continue
  (cd "$apk_work" && patch -p1 < "$patch_file" >/dev/null)
done

cat >"$cross" <<EOF
[binaries]
c = '$musl_cc'
ar = '$musl_ar'
strip = '$musl_strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF

env PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1 PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$sdk/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$sdk" \
  meson setup "$apk_build" "$apk_work" \
    --cross-file "$cross" \
    --prefix=/usr \
    --sysconfdir=/etc \
    --libdir=lib \
    --default-library=static \
    --prefer-static \
    -Dc_link_args=-static \
    -Dcrypto_backend=mbedtls \
    -Durl_backend=wget \
    -Dminimal=true \
    -Darch=aarch64 \
    -Dzstd=disabled \
    -Ddocs=disabled \
    -Dhelp=disabled \
    -Dlua=disabled \
    -Dpython=disabled \
    -Dtests=disabled \
    >/dev/null
meson compile -C "$apk_build" ./src/apk:executable
aarch64-unknown-linux-musl-strip -o "$out" "$apk_build/src/apk"
mv "$stamp_new" "$stamp"
