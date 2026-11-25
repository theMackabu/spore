#!/bin/sh
attempt=0
while [ "$attempt" -lt 100 ]; do
  if [ -S /nix/var/nix/daemon-socket/socket ] && /usr/bin/nix store info --store daemon >/dev/null 2>&1; then
    exit 0
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done
echo 'Nix daemon did not accept a store connection' >&2
exit 1
