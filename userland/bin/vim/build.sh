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
vim_src="$root/userland/third_party/vim"
vim_work="$build/vim-src"

mkdir -p "$build"
test -f "$nc_inst/lib/libncurses.so.6.4"

rm -rf "$vim_work"
mkdir -p "$vim_work"
git -C "$vim_src" archive --format=tar HEAD | tar -x -C "$vim_work"

(
  cd "$vim_work"
  vim_cv_uname_output=Linux \
  vim_cv_uname_r_output=6.0 \
  vim_cv_uname_m_output=aarch64 \
  vim_cv_toupper_broken=no \
  vim_cv_terminfo=yes \
  vim_cv_tgetent=zero \
  vim_cv_getcwd_broken=no \
  vim_cv_timer_create_works=yes \
  vim_cv_stat_ignores_slash=no \
  vim_cv_memmove_handles_overlap=yes \
    ./configure \
    --host=aarch64-unknown-linux-musl \
    --with-features=tiny \
    --disable-gui \
    --without-x \
    --disable-nls \
    --disable-acl \
    --disable-gpm \
    --disable-sysmouse \
    --disable-netbeans \
    --disable-channel \
    --disable-terminal \
    --with-tlib=ncurses \
    CPPFLAGS="-I$nc_inst/include -I$nc_inst/include/ncurses" \
    LDFLAGS="-L$nc_inst/lib -Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1" \
    LIBS="-lncurses" \
    CC=aarch64-unknown-linux-musl-gcc >/dev/null
)
make -C "$vim_work/src" -j"$jobs" vim >/dev/null
aarch64-unknown-linux-musl-strip -o "$out" "$vim_work/src/vim"
