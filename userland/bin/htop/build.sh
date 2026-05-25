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
nc_inst="$build/../../lib/ncurses/ncurses-install"
ht_src="$root/userland/third_party/htop"
ht_build="$build/htop-build"

mkdir -p "$build"
test -f "$nc_inst/lib/libncurses.so.6.4"

if [ ! -f "$ht_src/configure" ]; then
  (cd "$ht_src" && ./autogen.sh >/dev/null)
fi

rm -rf "$ht_build"
mkdir -p "$ht_build"
(
  cd "$ht_build"
  "$ht_src/configure" \
    --host=aarch64-unknown-linux-musl \
    --sysconfdir=/usr/local \
    --with-proc=/proc \
    --disable-unicode \
    --disable-affinity \
    --disable-hwloc \
    --disable-sensors \
    --disable-capabilities \
    --disable-delayacct \
    CPPFLAGS="-I$nc_inst/include -I$nc_inst/include/ncurses" \
    LDFLAGS="-L$nc_inst/lib -Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1" \
    LIBS="-lncurses" \
    CC=aarch64-unknown-linux-musl-gcc >/dev/null
)
make -C "$ht_build" -j"$jobs" htop >/dev/null
aarch64-unknown-linux-musl-strip -o "$out" "$ht_build/htop"
