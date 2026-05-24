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
ht_src="$root/userland/third_party/htop"
ht_build="$build/htop-build"
terminfo_dir="$build/terminfo"

mkdir -p "$build"

if [ ! -f "$nc_inst/lib/libncurses.a" ] || [ ! -f "$nc_inst/.spore-terminfo-v1" ]; then
  rm -rf "$nc_build" "$nc_inst"
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
      --without-shared \
      --with-normal \
      --without-termlib \
      --enable-termcap \
      --disable-db-install \
      --disable-widec \
      --without-fallbacks \
      --with-terminfo-dirs=/usr/share/terminfo:/etc/terminfo:/lib/terminfo >/dev/null
  )
  make -C "$nc_build" -j"$jobs" libs >/dev/null
  make -C "$nc_build" -j1 install.libs install.includes >/dev/null
  touch "$nc_inst/.spore-terminfo-v1"
fi

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
    --enable-static \
    --disable-unicode \
    --disable-affinity \
    --disable-hwloc \
    --disable-sensors \
    --disable-capabilities \
    --disable-delayacct \
    CPPFLAGS="-I$nc_inst/include -I$nc_inst/include/ncurses" \
    LDFLAGS="-L$nc_inst/lib -static" \
    LIBS="-lncurses" \
    CC=aarch64-unknown-linux-musl-gcc >/dev/null
)
make -C "$ht_build" -j"$jobs" htop >/dev/null
aarch64-unknown-linux-musl-strip -o "$out" "$ht_build/htop"

rm -rf "$terminfo_dir"
mkdir -p "$terminfo_dir"
if command -v infocmp >/dev/null 2>&1 && command -v tic >/dev/null 2>&1; then
  infocmp -x xterm-256color | tic -x -o "$terminfo_dir" /dev/stdin
  if [ -f "$terminfo_dir/78/xterm-256color" ] && [ ! -f "$terminfo_dir/x/xterm-256color" ]; then
    mkdir -p "$terminfo_dir/x"
    cp "$terminfo_dir/78/xterm-256color" "$terminfo_dir/x/xterm-256color"
  fi
else
  echo "htop: host tic/infocmp are required to build terminfo" >&2
  exit 1
fi
