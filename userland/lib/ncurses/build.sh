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
nc_src="$root/userland/third_party/ncurses"
nc_build="$build/ncurses-build"
nc_inst="$build/ncurses-install"
terminfo_dir="$build/terminfo"

rm -rf "$nc_build" "$nc_inst" "$terminfo_dir"
mkdir -p "$nc_build" "$nc_inst"
(
  cd "$nc_build"
  "$nc_src/configure" \
    --host=aarch64-unknown-linux-musl \
    --prefix="$nc_inst" \
    --with-build-cc=cc \
    --without-cxx \
    --without-ada \
    --without-manpages \
    --without-progs \
    --without-tests \
    --without-debug \
    --with-shared \
    --without-normal \
    --without-termlib \
    --enable-termcap \
    --disable-db-install \
    --disable-widec \
    --without-fallbacks \
    --with-terminfo-dirs=/usr/share/terminfo:/etc/terminfo:/lib/terminfo >/dev/null
)
make -C "$nc_build" -j"$jobs" libs >/dev/null
make -C "$nc_build" -j1 install.libs install.includes >/dev/null
aarch64-unknown-linux-musl-strip -o "$out" "$nc_inst/lib/libncurses.so.6.4"

mkdir -p "$terminfo_dir"
if command -v infocmp >/dev/null 2>&1 && command -v tic >/dev/null 2>&1; then
  infocmp -x xterm-256color | tic -x -o "$terminfo_dir" /dev/stdin
  if [ -f "$terminfo_dir/78/xterm-256color" ] && [ ! -f "$terminfo_dir/x/xterm-256color" ]; then
    mkdir -p "$terminfo_dir/x"
    cp "$terminfo_dir/78/xterm-256color" "$terminfo_dir/x/xterm-256color"
  fi
else
  echo "ncurses: host tic/infocmp are required to build terminfo" >&2
  exit 1
fi
