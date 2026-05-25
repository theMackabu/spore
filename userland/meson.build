userland_musl_cc = find_program('aarch64-unknown-linux-musl-gcc')
userland_include_arg = '-I' + meson.current_source_dir() / 'lib/spore'

userland_cc = [
  userland_musl_cc,
  '-std=c23',
  '-D_GNU_SOURCE',
  '-s',
  '-Wl,-dynamic-linker,/lib/ld-musl-aarch64.so.1',
  '-Wl,-rpath,/lib',
  userland_include_arg,
]
userland_static_cc = [
  userland_musl_cc,
  '-static',
  '-std=c23',
  '-D_GNU_SOURCE',
  '-s',
  userland_include_arg,
]
userland_image_targets = []
userland_test_targets = []
userland_image_data = []

subdir('etc')
userland_image_data += files('usr/local/htoprc')
userland_image_data += files('demos/something', 'demos/something-else.msh')
subdir('lib/ncurses')
subdir('lib/spore')

subdir('bin/init')
subdir('bin/msh')
subdir('bin/dash')
subdir('bin/myc')
subdir('bin/shutdown')
subdir('bin/reboot')
subdir('bin/ls')
subdir('bin/cat')
subdir('bin/echo')
subdir('bin/clear')
subdir('bin/file')
subdir('bin/stat')
subdir('bin/uname')
subdir('bin/edit')
subdir('bin/pico')
subdir('bin/head')
subdir('bin/tail')
subdir('bin/wc')
subdir('bin/grep')
subdir('bin/find')
subdir('bin/sort')
subdir('bin/uniq')
subdir('bin/cut')
subdir('bin/tr')
subdir('bin/sed-lite')
subdir('bin/tar')
subdir('bin/ps')
subdir('bin/top')
subdir('bin/df')
subdir('bin/free')
subdir('bin/dirname')
subdir('bin/basename')
subdir('bin/sleep')
subdir('bin/date')
subdir('bin/uptime')
subdir('bin/whoami')
subdir('bin/hostname')
subdir('bin/hostid')
subdir('bin/id')
subdir('bin/who')
subdir('bin/tee')
subdir('bin/logger')
subdir('bin/printenv')
subdir('bin/printf')
subdir('bin/tty')
subdir('bin/yes')
subdir('bin/cp')
subdir('bin/mv')
subdir('bin/ln')
subdir('bin/chmod')
subdir('bin/chgrp')
subdir('bin/chown')
subdir('bin/chroot')
subdir('bin/mkuser')
subdir('bin/deluser')
subdir('bin/passwd')
subdir('bin/su')
subdir('bin/sudo')
subdir('bin/groups')
subdir('bin/kill')
subdir('bin/du')
subdir('bin/hexdump')
subdir('bin/xxd')
subdir('bin/base64')
subdir('bin/cksum')
subdir('bin/sum')
subdir('bin/sha1sum')
subdir('bin/readlink')
subdir('bin/time')
subdir('bin/mk')
subdir('bin/mkfifo')
subdir('bin/mkdir')
subdir('bin/rm')
subdir('bin/touch')
subdir('bin/pwd')
subdir('bin/hello')
subdir('bin/true')
subdir('bin/false')
subdir('bin/pthread-demo')
subdir('bin/ping')
subdir('bin/udp-echo')
subdir('bin/udp-send')
subdir('bin/udp-recv')
subdir('bin/uds-server')
subdir('bin/uds-client')

subdir('usr/bin/env')
subdir('usr/local/bin/fastfetch')
subdir('usr/local/bin/htop')
subdir('usr/local/bin/nano')
subdir('usr/local/bin/tcc')
subdir('usr/local/bin/vim')

subdir('demos/spinner')
subdir('demos/peeker')
subdir('demos/writer')
subdir('demos/memhog')
subdir('demos/escalate')
subdir('demos/signal-crash')

subdir('tests/integration')
