#!/bin/dash
case "$1" in
  -I|--all-ip-addresses) exec /bin/dhcpc --address ;;
esac
/libexec/hostname "$@" || exit $?
case "$1" in
  -F|--file) ;;
  ''|-*) exit 0 ;;
esac
name=$(/libexec/hostname) || exit 1
IFS= read -r saved < /etc/hostname || saved=
if [ "$name" != "$saved" ]; then
  printf '%s\n' "$name" > /etc/hostname
fi
