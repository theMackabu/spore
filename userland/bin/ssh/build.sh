#!/bin/sh
set -eu

if [ "$#" -ne 6 ]; then
  echo "usage: build.sh SOURCE_ROOT BUILD_DIR SSH_OUT DBCLIENT_OUT DROPBEARKEY_OUT DROPBEARCONVERT_OUT" >&2
  exit 2
fi

root=$1
build=$2
ssh_out=$3
dbclient_out=$4
dropbearkey_out=$5
dropbearconvert_out=$6

case "$ssh_out" in /*) ;; *) ssh_out="$PWD/$ssh_out" ;; esac
case "$dbclient_out" in /*) ;; *) dbclient_out="$PWD/$dbclient_out" ;; esac
case "$dropbearkey_out" in /*) ;; *) dropbearkey_out="$PWD/$dropbearkey_out" ;; esac
case "$dropbearconvert_out" in /*) ;; *) dropbearconvert_out="$PWD/$dropbearconvert_out" ;; esac

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
dropbear_src="$root/userland/third_party/dropbear"
work="$build/dropbear-src"
obj="$build/dropbear-build"
stamp="$build/.dropbear-client.stamp"
stamp_new="$build/.dropbear-client.stamp.new"

mkdir -p "$build"
{
  git -C "$dropbear_src" rev-parse HEAD
  cksum "$0"
  aarch64-unknown-linux-musl-gcc --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$ssh_out" ] && [ -f "$dbclient_out" ] && [ -f "$dropbearkey_out" ] && [ -f "$dropbearconvert_out" ] &&
   [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$work" "$obj"
mkdir -p "$work" "$obj"
git -C "$dropbear_src" archive --format=tar HEAD | tar -x -C "$work"
perl -0pi -e 's/dropbear_exit\("Error in select"\);/dropbear_exit("Error in select nfds=%d errno=%d %s", ses.maxfd + 1, errno, strerror(errno));/' "$work/src/common-session.c"
perl -0pi -e 's/hints\.ai_family = AF_UNSPEC;/hints.ai_family = AF_INET;/g' "$work/src/netio.c"

(
  cd "$obj"
  "$work/configure" \
    --host=aarch64-unknown-linux-musl \
    --enable-static \
    --enable-bundled-libtom \
    --disable-zlib \
    --disable-syslog \
    --disable-lastlog \
    --disable-utmp \
    --disable-utmpx \
    --disable-wtmp \
    --disable-wtmpx \
    --disable-loginfunc \
    --disable-pututline \
    --disable-pututxline \
    CC=aarch64-unknown-linux-musl-gcc \
    CFLAGS="-Os -DDROPBEAR_KEX_FIRST_FOLLOWS=0" \
    LDFLAGS="-static" >"$build/dropbear-configure.log" 2>&1
  cat > localoptions.h <<'EOF'
#define DROPBEAR_MLKEM768 0
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_KEXGUESS2 0
EOF
)
if ! make -C "$obj" -j"$jobs" PROGRAMS="dbclient dropbearkey dropbearconvert" >"$build/dropbear-build.log" 2>&1; then
  cat "$build/dropbear-build.log" >&2
  exit 1
fi

cp "$obj/dbclient" "$ssh_out"
cp "$obj/dbclient" "$dbclient_out"
cp "$obj/dropbearkey" "$dropbearkey_out"
cp "$obj/dropbearconvert" "$dropbearconvert_out"
aarch64-unknown-linux-musl-strip "$ssh_out" "$dbclient_out" "$dropbearkey_out" "$dropbearconvert_out"

mv "$stamp_new" "$stamp"
