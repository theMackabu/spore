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
curl_src="$root/userland/third_party/curl"
curl_build="$build/curl-build"
stamp="$build/.curl.stamp"
stamp_new="$build/.curl.stamp.new"

mkdir -p "$build"
{
  git -C "$curl_src" rev-parse HEAD
  cksum "$0"
  aarch64-unknown-linux-musl-gcc --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$out" ] && [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$curl_build"
mkdir -p "$curl_build"

(
  cd "$curl_build"
  cmake "$curl_src" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-unknown-linux-musl-gcc \
    -DCMAKE_AR=/run/current-system/sw/bin/aarch64-unknown-linux-musl-ar \
    -DCMAKE_RANLIB=/run/current-system/sw/bin/aarch64-unknown-linux-musl-ranlib \
    -DCMAKE_STRIP=/run/current-system/sw/bin/aarch64-unknown-linux-musl-strip \
    -DCMAKE_EXE_LINKER_FLAGS=-static \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
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
    -DCURL_ENABLE_SSL=OFF \
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
