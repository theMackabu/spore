#!/bin/dash
if [ "$#" -ne 1 ]; then
  echo 'usage: deluser NAME' >&2
  exit 2
fi
case "$1" in ''|-*|root) echo 'deluser: invalid or protected account' >&2; exit 1 ;; esac
exec /usr/sbin/userdel "$1"
