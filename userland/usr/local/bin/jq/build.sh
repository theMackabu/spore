#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: build.sh SOURCE_ROOT BUILD_DIR OUTPUT" >&2
  exit 2
fi

root=$1
build=$2
out=$3
case "$out" in
  /*) ;;
  *) out="$PWD/$out" ;;
esac

jq_src_root="$root/userland/third_party/jq"
onig_src_root="$jq_src_root/vendor/oniguruma"
work="$build/jq-src"
stamp="$build/.jq.stamp"
stamp_new="$build/.jq.stamp.new"

mkdir -p "$build"
{
  git -C "$jq_src_root" rev-parse HEAD
  git -C "$onig_src_root" rev-parse HEAD
  cksum "$0"
  aarch64-unknown-linux-musl-gcc --version | sed -n '1p'
} >"$stamp_new"

if [ -f "$out" ] && [ -f "$stamp" ] && cmp -s "$stamp_new" "$stamp"; then
  rm -f "$stamp_new"
  exit 0
fi

rm -rf "$work"
mkdir -p "$work/vendor/oniguruma"
git -C "$jq_src_root" archive --format=tar HEAD | tar -x -C "$work"
git -C "$onig_src_root" archive --format=tar HEAD | tar -x -C "$work/vendor/oniguruma"

cd "$work"
mkdir -p src build-config
printf '#define JQ_VERSION "jq-1.8.1-spore"\n' >src/version.h
printf '#define JQ_CONFIG "spore direct build"\n' >src/config_opts.inc
od -v -A n -t o1 -- src/builtin.jq | \
  sed -e 's/$/ /' \
      -e 's/\([0123456789]\) /\1, /g' \
      -e 's/ $//' \
      -e 's/ 0/  0/g' \
      -e 's/ \([123456789]\)/ 0\1/g' >src/builtin.inc
cat >build-config/config.h <<'EOF'
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ALLOCA_H 1
#define STDC_HEADERS 1
EOF

jq_sources="
src/main.c src/builtin.c src/bytecode.c src/compile.c src/execute.c
src/jq_test.c src/jv.c src/jv_alloc.c src/jv_aux.c src/jv_dtoa.c
src/jv_file.c src/jv_parse.c src/jv_print.c src/jv_unicode.c
src/linker.c src/locfile.c src/util.c src/jv_dtoa_tsd.c
src/parser.c src/lexer.c vendor/decNumber/decContext.c
vendor/decNumber/decNumber.c
"
onig_sources="
vendor/oniguruma/src/regparse.c vendor/oniguruma/src/regcomp.c
vendor/oniguruma/src/regexec.c vendor/oniguruma/src/regenc.c
vendor/oniguruma/src/regerror.c vendor/oniguruma/src/regext.c
vendor/oniguruma/src/regsyntax.c vendor/oniguruma/src/regtrav.c
vendor/oniguruma/src/regversion.c vendor/oniguruma/src/st.c
vendor/oniguruma/src/reggnu.c vendor/oniguruma/src/unicode.c
vendor/oniguruma/src/unicode_unfold_key.c vendor/oniguruma/src/unicode_fold1_key.c
vendor/oniguruma/src/unicode_fold2_key.c vendor/oniguruma/src/unicode_fold3_key.c
vendor/oniguruma/src/ascii.c vendor/oniguruma/src/utf8.c
vendor/oniguruma/src/utf16_be.c vendor/oniguruma/src/utf16_le.c
vendor/oniguruma/src/utf32_be.c vendor/oniguruma/src/utf32_le.c
vendor/oniguruma/src/euc_jp.c vendor/oniguruma/src/euc_jp_prop.c
vendor/oniguruma/src/sjis.c vendor/oniguruma/src/sjis_prop.c
vendor/oniguruma/src/iso8859_1.c vendor/oniguruma/src/iso8859_2.c
vendor/oniguruma/src/iso8859_3.c vendor/oniguruma/src/iso8859_4.c
vendor/oniguruma/src/iso8859_5.c vendor/oniguruma/src/iso8859_6.c
vendor/oniguruma/src/iso8859_7.c vendor/oniguruma/src/iso8859_8.c
vendor/oniguruma/src/iso8859_9.c vendor/oniguruma/src/iso8859_10.c
vendor/oniguruma/src/iso8859_11.c vendor/oniguruma/src/iso8859_13.c
vendor/oniguruma/src/iso8859_14.c vendor/oniguruma/src/iso8859_15.c
vendor/oniguruma/src/iso8859_16.c vendor/oniguruma/src/euc_tw.c
vendor/oniguruma/src/euc_kr.c vendor/oniguruma/src/big5.c
vendor/oniguruma/src/gb18030.c vendor/oniguruma/src/koi8_r.c
vendor/oniguruma/src/cp1251.c vendor/oniguruma/src/onig_init.c
"

# jq's release build normally gets these from configure. Keep the list local to
# the wrapper so the upstream submodule stays untouched.
aarch64-unknown-linux-musl-gcc \
  -O2 -std=c99 -D_GNU_SOURCE -DIEEE_8087 \
  -DHAVE_LIBONIG -DHAVE_ISATTY -DHAVE_SETLOCALE -DHAVE_STRPTIME \
  -DHAVE_STRFTIME -DHAVE_GETTIMEOFDAY -DHAVE_GMTIME_R -DHAVE_LOCALTIME_R \
  -DHAVE_TIMEGM -DHAVE___THREAD -DHAVE_PTHREAD -DHAVE_PTHREAD_KEY_CREATE \
  -DHAVE_PTHREAD_ONCE -DHAVE_ATEXIT -DHAVE_MEMMEM -DHAVE_ALLOCA_H \
  -DHAVE_ACOS -DHAVE_ASIN -DHAVE_ATAN -DHAVE_ATAN2 -DHAVE_CBRT -DHAVE_COS \
  -DHAVE_COSH -DHAVE_EXP -DHAVE_EXP2 -DHAVE_FLOOR -DHAVE_HYPOT -DHAVE_LOG \
  -DHAVE_LOG10 -DHAVE_LOG2 -DHAVE_POW -DHAVE_REMAINDER -DHAVE_SIN \
  -DHAVE_SINH -DHAVE_SQRT -DHAVE_TAN -DHAVE_TANH -DHAVE_TGAMMA -DHAVE_CEIL \
  -DHAVE_COPYSIGN -DHAVE_ERF -DHAVE_ERFC -DHAVE_EXPM1 -DHAVE_FABS \
  -DHAVE_FDIM -DHAVE_FMA -DHAVE_FMAX -DHAVE_FMIN -DHAVE_FMOD -DHAVE_LGAMMA \
  -DHAVE_LOG1P -DHAVE_NEARBYINT -DHAVE_NEXTAFTER -DHAVE_RINT -DHAVE_ROUND \
  -DHAVE_TRUNC -DHAVE_LDEXP -DHAVE_MODF -DHAVE_FREXP -DHAVE_LGAMMA_R \
  -Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1 -Wl,-rpath,/lib \
  -I. -Isrc -Ivendor -Ivendor/oniguruma/src -Ivendor/decNumber -Ibuild-config \
  $jq_sources $onig_sources -lm -pthread -o "$out"
aarch64-unknown-linux-musl-strip "$out"
mv "$stamp_new" "$stamp"
