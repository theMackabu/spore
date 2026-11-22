#!/bin/sh
set -eu

work=$(mktemp -d)
trap 'rm -r "$work"' EXIT
infocmp -x xterm-256color > "$work/entries.src"
cat "$1" >> "$work/entries.src"
tic -x -o "$work/db" "$work/entries.src"

# tic indexes entries by character on Linux and by hex on some macOS hosts.
if [ -f "$work/db/x/xterm-256color" ]; then
  cp "$work/db/x/xterm-256color" "$2"
  cp "$work/db/s/spore" "$3"
else
  cp "$work/db/78/xterm-256color" "$2"
  cp "$work/db/73/spore" "$3"
fi
