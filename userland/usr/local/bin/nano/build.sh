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
cleaner="$build/elf-clean-runpath"
nc_inst="$build/../../../../lib/ncurses/ncurses-install"
nano_src="$root/userland/third_party/nano"
nano_work="$build/nano-src"
nano_build="$build/nano-build"
wrap_dir="$build/aclocal-wrap"
aclocal_extra="$build/aclocal-extra"
gnulib_hash="b75134c814c38876f04029ffc3fae4e90035dc34"
gnulib_cache="$build/gnulib-cache"

mkdir -p "$build"
test -f "$nc_inst/lib/libncurses.so.6.4"
cc "$root/tools/src/elf_clean_runpath.c" -o "$cleaner"

rm -rf "$nano_work" "$nano_build" "$wrap_dir" "$aclocal_extra"
mkdir -p "$nano_work" "$nano_build" "$wrap_dir" "$aclocal_extra"
git -C "$nano_src" archive --format=tar HEAD | tar -x -C "$nano_work"

if [ ! -d "$gnulib_cache/.git" ]; then
  rm -rf "$gnulib_cache"
  git clone --depth=2222 https://git.savannah.gnu.org/git/gnulib.git "$gnulib_cache" >/dev/null
fi
git -C "$gnulib_cache" fetch --depth=2222 origin "$gnulib_hash" >/dev/null 2>&1 || true
git -C "$gnulib_cache" checkout --force "$gnulib_hash" >/dev/null
cp -R "$gnulib_cache" "$nano_work/gnulib"

pkg_m4=$(find /nix/store /opt/homebrew /usr/local /run/current-system/sw -path '*/pkg.m4' -print 2>/dev/null | head -1 || true)
if [ -z "$pkg_m4" ]; then
  echo "nano: pkg.m4 is required to run autogen.sh" >&2
  exit 1
fi
cp "$pkg_m4" "$aclocal_extra/pkg.m4"
real_aclocal=$(command -v aclocal)
{
  printf '#!/bin/sh\n'
  printf 'if [ "$1" = "--print-ac-dir" ]; then echo "%s"; exit 0; fi\n' "$aclocal_extra"
  printf 'exec "%s" -I "%s" "$@"\n' "$real_aclocal" "$aclocal_extra"
} >"$wrap_dir/aclocal"
chmod +x "$wrap_dir/aclocal"

(
  cd "$nano_work"
  PATH="$wrap_dir:$PATH" ./autogen.sh >/dev/null
)
(
  cd "$nano_build"
  PATH="$wrap_dir:$PATH" "$nano_work/configure" \
    --host=aarch64-unknown-linux-musl \
    --prefix=/usr \
    --enable-tiny \
    --disable-nls \
    --disable-utf8 \
    --disable-libmagic \
    --disable-browser \
    --disable-help \
    --disable-speller \
    --disable-mouse \
    --disable-nanorc \
    --disable-wrapping \
    CPPFLAGS="-I$nc_inst/include -I$nc_inst/include/ncurses" \
    LDFLAGS="-L$nc_inst/lib -Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1" \
    LIBS="-lncurses" \
    CC=aarch64-unknown-linux-musl-gcc >/dev/null
)
make -C "$nano_build" -j"$jobs" >/dev/null
aarch64-unknown-linux-musl-strip -o "$out" "$nano_build/src/nano"
"$cleaner" "$out"
