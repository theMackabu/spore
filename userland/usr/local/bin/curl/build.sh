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

# Spore curl is HTTP(S)-only, static-musl, and uses mbedTLS plus the baked
# Mozilla CA bundle at /etc/ssl/certs/ca-certificates.crt. Build for network/TLS
# runtime speed; the image can afford a slightly larger curl.

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
curl_src="$root/userland/third_party/curl"
curl_work_src="$build/curl-src"
curl_build="$build/curl-build"
patch_dir="$root/userland/usr/local/bin/curl/patches"
mbedtls_inst="$project_build/userland/lib/mbedtls/mbedtls-install"
stamp="$build/.curl.stamp"
stamp_new="$build/.curl.stamp.new"

mkdir -p "$build"
{
  git -C "$curl_src" rev-parse HEAD
  test -f "$mbedtls_inst/lib/libmbedtls.a" && cksum "$mbedtls_inst/lib/libmbedtls.a"
  if [ -d "$patch_dir" ]; then
    find "$patch_dir" -type f -name '*.patch' -print | sort | while IFS= read -r patch_file; do
      cksum "$patch_file"
    done
  fi
  cksum "$0"
  aarch64-unknown-linux-musl-gcc --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$out" ] && [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$curl_build" "$curl_work_src"
mkdir -p "$curl_build"
cp -R "$curl_src" "$curl_work_src"
rm -rf "$curl_work_src/.git"
if [ -d "$patch_dir" ]; then
  for patch_file in "$patch_dir"/*.patch; do
    [ -e "$patch_file" ] || continue
    patch -d "$curl_work_src" -p1 <"$patch_file" >/dev/null
  done
fi

(
  cd "$curl_build"
  cmake "$curl_work_src" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-unknown-linux-musl-gcc \
    -DCMAKE_AR=/run/current-system/sw/bin/aarch64-unknown-linux-musl-ar \
    -DCMAKE_RANLIB=/run/current-system/sw/bin/aarch64-unknown-linux-musl-ranlib \
    -DCMAKE_STRIP=/run/current-system/sw/bin/aarch64-unknown-linux-musl-strip \
    -DCMAKE_EXE_LINKER_FLAGS=-static \
    -DCMAKE_PREFIX_PATH="$mbedtls_inst" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=armv8-a+crypto" \
    -DBUILD_CURL_EXE=ON \
    -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_STATIC_CURL=ON \
    -DBUILD_STATIC_LIBS=ON \
    -DCURL_DISABLE_INSTALL=ON \
    -DCURL_DISABLE_LIBCURL_OPTION=ON \
    -DCURL_DISABLE_COOKIES=ON \
    -DCURL_DISABLE_NETRC=ON \
    -DCURL_DISABLE_PROGRESS_METER=ON \
    -DCURL_DISABLE_VERBOSE_STRINGS=ON \
    -DCURL_ENABLE_SSL=ON \
    -DCURL_USE_MBEDTLS=ON \
    -DCURL_DEFAULT_SSL_BACKEND=mbedtls \
    -DMBEDTLS_USE_STATIC_LIBS=ON \
    -DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt \
    -DCURL_CA_PATH=none \
    -DENABLE_IPV6=OFF \
    -DCURL_USE_LIBPSL=OFF \
    -DCURL_USE_LIBSSH2=OFF \
    -DENABLE_CURL_MANUAL=OFF \
    -DENABLE_THREADED_RESOLVER=OFF \
    -DHTTP_ONLY=ON \
    -DHAVE_GETHOSTBYNAME_R=OFF \
    -DUSE_LIBIDN2=OFF \
    -DUSE_NGHTTP2=OFF \
    -DUSE_NGTCP2=OFF \
    -DUSE_NGHTTP3=OFF \
    -DZLIB_FOUND=OFF \
    -DBROTLI_FOUND=OFF \
    -DZSTD_FOUND=OFF \
    >/dev/null
)
cmake --build "$curl_build" --target curl -j "$jobs" >/dev/null
aarch64-unknown-linux-musl-strip -o "$out" "$curl_build/src/curl"
mv "$stamp_new" "$stamp"
