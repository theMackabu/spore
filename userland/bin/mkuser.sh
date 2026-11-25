#!/bin/sh
if [ "${1-}" = --no-password ]; then
  if [ "$#" -ne 2 ]; then
    echo 'usage: mkuser --no-password NAME' >&2
    exit 2
  fi
  exec /usr/sbin/useradd -p '!' -- "$2"
fi
if [ "$#" -ne 1 ] || [ ! -t 0 ]; then
  exec /usr/sbin/useradd "$@"
fi
case "$1" in ''|-*) exec /usr/sbin/useradd "$@" ;; esac
/usr/sbin/useradd "$1" || exit $?
exec /bin/passwd "$1"
