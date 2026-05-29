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

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
apk_src="$root/userland/third_party/apk-tools"
mbedtls_src="$root/userland/third_party/mbedtls"
apk_work="$build/apk-tools-src"
apk_build="$build/apk-tools-build"
mbedtls_work="$build/mbedtls3-src"
mbedtls_build="$build/mbedtls3-build"
mbedtls_inst="$build/mbedtls3-install"
zlib_inst="$project_build/userland/lib/zlib/zlib-install"
cross="$build/apk-cross.ini"
stamp="$build/.apk.stamp"
stamp_new="$build/.apk.stamp.new"

mkdir -p "$build"
{
  git -C "$apk_src" rev-parse HEAD
  git -C "$mbedtls_src" rev-parse mbedtls-3.6.6^{commit}
  git -C "$mbedtls_src/framework" rev-parse dff9da04438d712f7647fd995bc90fadd0c0e2ce^{commit}
  test -f "$zlib_inst/lib/libz.a" && cksum "$zlib_inst/lib/libz.a"
  cksum "$0"
  cksum "$root"/userland/usr/local/bin/apk/patches/*.patch
  aarch64-unknown-linux-musl-gcc --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$out" ] && [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$apk_work" "$apk_build" "$mbedtls_work" "$mbedtls_build" "$mbedtls_inst"
mkdir -p "$apk_work" "$apk_build" "$mbedtls_work" "$mbedtls_build" "$mbedtls_inst"
git -C "$apk_src" archive --format=tar HEAD | tar -x -C "$apk_work"
git -C "$mbedtls_src" archive --format=tar mbedtls-3.6.6 | tar -x -C "$mbedtls_work"
mkdir -p "$mbedtls_work/framework"
git -C "$mbedtls_src/framework" archive --format=tar dff9da04438d712f7647fd995bc90fadd0c0e2ce | tar -x -C "$mbedtls_work/framework"

# Spore does not implement Linux O_TMPFILE yet. Force apk-tools onto its
# named-temporary-file fallback in the build copy.
sed -i.bak 's|#define HAVE_O_TMPFILE|/* #undef HAVE_O_TMPFILE */|g' "$apk_work/src/io.c"

for patch_file in "$root"/userland/usr/local/bin/apk/patches/*.patch; do
  [ -e "$patch_file" ] || continue
  (cd "$apk_work" && patch -p1 < "$patch_file" >/dev/null)
done

# apk-tools 3.0.6 models the mbedTLS backend as an array of dependencies, but
# it expects the classic mbedTLS 3 public bignum API. Spore's main TLS stack
# tracks newer mbedTLS, so apk gets a private static mbedTLS 3 build here.
(
  cd "$mbedtls_build"
  cmake -Wno-dev "$mbedtls_work" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-unknown-linux-musl-gcc \
    -DCMAKE_AR=/run/current-system/sw/bin/aarch64-unknown-linux-musl-ar \
    -DCMAKE_RANLIB=/run/current-system/sw/bin/aarch64-unknown-linux-musl-ranlib \
    -DCMAKE_INSTALL_PREFIX="$mbedtls_inst" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=armv8-a+crypto" \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DLINK_WITH_PTHREAD=OFF \
    -DDISABLE_PACKAGE_CONFIG_AND_INSTALL=OFF \
    >/dev/null
)
cmake --build "$mbedtls_build" -j "$jobs" >/dev/null
cmake --install "$mbedtls_build" >/dev/null

cat >"$cross" <<EOF
[binaries]
c = 'aarch64-unknown-linux-musl-gcc'
ar = '/run/current-system/sw/bin/aarch64-unknown-linux-musl-ar'
strip = '/run/current-system/sw/bin/aarch64-unknown-linux-musl-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF

env PKG_CONFIG_PATH="$zlib_inst/lib/pkgconfig:$mbedtls_inst/lib/pkgconfig" \
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
meson compile -C "$apk_build" ./src/apk:executable >/dev/null
aarch64-unknown-linux-musl-strip -o "$out" "$apk_build/src/apk"
mv "$stamp_new" "$stamp"
