#!/bin/sh
set -eu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$1" "$tmp/spore"
cd "$tmp"
mkdir -p tree/etc tree/bin tree/home/spore tree/lib/apk/db
printf 'v1' > tree/etc/clean
printf 'v1' > tree/etc/edited
printf 'v1' > tree/etc/deleted
printf 'v1' > tree/etc/retired
printf 'v1' > tree/bin/tool
ln -s dash tree/bin/sh
printf 'P:seed\nV:1\n\n' > tree/lib/apk/db/installed
touch image.img

image() {
    mke2fs -q -t ext2 -b 4096 -U "$1" -d tree "$2" 2048
}
oldid=95104634-47e7-4345-bc66-8f3aef51e01a
image "$oldid" previous.ext2
cp -R tree next
printf 'local' > tree/etc/edited
printf 'handwritten' > tree/etc/edited.spore-new
printf 'repo' > tree/home/spore/work
printf 'P:my-tool\nV:1\nF:bin\nR:tool\n' >> tree/lib/apk/db/installed
rm tree/etc/deleted
image "$oldid" run-root.ext2
before=$(cksum run-root.ext2)
mv tree local
mv next tree
printf 'v2' > tree/etc/clean
printf 'v2' > tree/etc/edited
printf 'v2' > tree/bin/tool
printf 'P:seed\nV:2\n\n' > tree/lib/apk/db/installed
printf 'added' > tree/etc/new
rm tree/etc/retired tree/bin/sh
ln -s bash tree/bin/sh
image 73c4c765-91e5-4cb0-814d-9d01c6b614bc root.ext2
if ./spore upgrade --from-base root.ext2; then exit 1; fi
test ! -e system/current
./spore upgrade --from-base previous.ext2
qemu-img convert -f qcow2 -O raw system/current/data.qcow2 actual.ext2
value() { debugfs -R "cat $1" actual.ext2 2>/dev/null; }
test "$(value /home/spore/work)" = repo
test "$(value /etc/clean)" = v2
test "$(value /etc/edited)" = local
test "$(value /etc/edited.spore-new)" = handwritten
test "$(value /etc/edited.spore-new.1)" = v2
test "$(value /etc/new)" = added
test "$(value /bin/tool)" = v1
test "$(value /bin/tool.spore-new)" = v2
value /lib/apk/db/installed | grep -q '^P:my-tool$'
value /lib/apk/db/installed | grep -q '^V:2$'
debugfs -R 'stat /bin/sh' actual.ext2 2>/dev/null | grep -q '"bash"'
for name in deleted retired; do
    debugfs -R "stat /etc/$name" actual.ext2 2>&1 | grep -qi 'file not found'
done
first=$(readlink system/current)
mv system/current/data.qcow2 paused.qcow2
if ./spore upgrade; then exit 1; fi
test "$(readlink system/current)" = "$first"
mv paused.qcow2 system/current/data.qcow2
./spore upgrade
test "$(readlink system/current)" = "$first"
printf 'v3' > tree/etc/clean
rm root.ext2
image 73c4c765-91e5-4cb0-814d-9d01c6b614bc root.ext2
./spore upgrade
qemu-img convert -f qcow2 -O raw system/current/data.qcow2 actual.ext2
test "$(value /etc/clean)" = v3
test "$(value /etc/edited)" = local
./spore rollback
test "$(readlink system/current)" = "$first"
./spore rollback
test ! -e system/current
test "$before" = "$(cksum run-root.ext2)"
