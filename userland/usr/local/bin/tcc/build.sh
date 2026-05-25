#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: build.sh SOURCE_ROOT BUILD_DIR TCC_OUTPUT LIBTCC1_OUTPUT" >&2
  exit 2
fi

root=$1
build=$2
out_tcc=$3
out_libtcc1=$4

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
tcc_src="$root/userland/third_party/tinycc"
tcc_work="$build/tcc-src"
tcc_build="$build/tcc-build"
stamp="$build/.tcc.stamp"
stamp_new="$build/.tcc.stamp.new"

mkdir -p "$build"
{
  git -C "$tcc_src" rev-parse HEAD
  cksum "$0"
  aarch64-unknown-linux-musl-gcc --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$out_tcc" ] && [ -f "$out_libtcc1" ] && [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$tcc_work" "$tcc_build"
mkdir -p "$tcc_work" "$tcc_build"
git -C "$tcc_src" archive --format=tar HEAD | tar -x -C "$tcc_work"

# TinyCC's AArch64 runtime still names the old GCC helper directly. Build the
# guest libtcc1 with the cross compiler and use its builtin cache helper.
sed -i.bak 's/__arm64_clear_cache(beg, end);/__builtin___clear_cache((char *)beg, (char *)end);/' \
  "$tcc_work/lib/armflush.c"

(
  cd "$tcc_build"
  "$tcc_work/configure" \
    --cc=aarch64-unknown-linux-musl-gcc \
    --ar=aarch64-unknown-linux-musl-ar \
    --cpu=arm64 \
    --targetos=Linux \
    --config-musl \
    --prefix=/usr/local \
    --bindir=/usr/local/bin \
    --libdir=/usr/local/lib \
    --tccdir=/usr/local/lib/tcc \
    --includedir=/usr/include \
    --sysincludepaths=/usr/local/include:/usr/include \
    --libpaths=/usr/local/lib/tcc:/usr/local/lib:/usr/lib:/lib \
    --crtprefix=/usr/lib \
    --elfinterp=/lib/ld-musl-aarch64.so.1 \
    --extra-ldflags=-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1 >/dev/null
  cc -DC2STR "$tcc_work/conftest.c" -o c2str.exe
  ./c2str.exe "$tcc_work/include/tccdefs.h" tccdefs_.h
  make -j"$jobs" tcc >/dev/null
  make -C lib arm64-libtcc1-usegcc=yes >/dev/null
)

aarch64-unknown-linux-musl-strip -o "$out_tcc" "$tcc_build/tcc"
cp "$tcc_build/libtcc1.a" "$out_libtcc1"
mv "$stamp_new" "$stamp"
