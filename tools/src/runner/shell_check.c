#include "run_modes.h"
#include "runner.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *shell_commands[] = {
  "ls /bin\n",
  "ls /dev\n",
  "ls /dev/fs\n",
  "ls /dev/blk\n",
  "ls /proc\n",
  "cat /proc/procinfo\n",
  "cat /proc/meminfo\n",
  "cat /proc/mounts\n",
  "cat /proc/filesystems\n",
  "cat /proc/partitions\n",
  "cat /proc/devices\n",
  "stat /dev/null /dev/blk/root /proc/mounts /tmp\n",
  "echo null-ok > /dev/null\n",
  "echo console-ok > /dev/console\n",
  "echo full > /dev/full\n",
  "cat /etc/motd\n",
  "uname -a\n",
  "/bin/dash -c 'fastfetch || echo __SPORE\"\"_SHELL_CHECK_FAIL_FASTFETCH__'\n",
  "myc status\n",
  "myc list-units\n",
  "myc list-dependencies multi-user.target\n",
  "myc status network.service\n",
  "cat /etc/resolv.conf\n",
  "hostname\n",
  "hostname -s\n",
  "hostname -d\n",
  "hostname -i\n",
  "hostname -I\n",
  "ping -c 1 127.0.0.1\n",
  "nslookup localhost\n",
  "nslookup example.com\n",
  "printf '10.0.2.2 example.test\\n' | sudo tee -a /etc/hosts\n",
  "curl http://example.test:8080/\n",
  "/bin/dash -c 'which wget && wget -qO /tmp/download "
  "http://10.0.2.2:8080/ && grep \"hello from host http\" /tmp/download || echo __SPORE\"\"_SHELL_CHECK_FAIL_WGET__'\n",
  "printf '#include <stdio.h>\\nint main(void) { puts(\"compiler-ok\"); return 0; }\\n' > /tmp/check.c\n",
  "/bin/dash -c 'vi -es -c \"w! /tmp/edited.c\" -c qa! /tmp/check.c && "
  "cc /tmp/edited.c -o ./cc-check && ./cc-check | grep compiler-ok || echo __SPORE\"\"_SHELL_CHECK_FAIL_CC__'\n",
  "curl -sS https://example.com/ > /tmp/https-example\n",
  "grep 'Example Domain' /tmp/https-example\n",
  "p=__SPORE; q=_SHELL_CHECK_FAIL_TLS_BADCERT_ACCEPTED__; curl -fsS https://expired.badssl.com/ >/tmp/badcert "
  "2>/tmp/badcert.err && echo \"$p$q\"\n",
  "/bin/dash -c 'grep certificate /tmp/badcert.err || { p=__SPORE; q=_SHELL_CHECK_FAIL_TLS_BADCERT_REASON__; echo "
  "\"$p$q\"; }'\n",
  "p=__SPORE; q=_SHELL_CHECK_FAIL_TLS_HOSTNAME_ACCEPTED__; curl -fsS https://wrong.host.badssl.com/ >/tmp/badhost "
  "2>/tmp/badhost.err && echo \"$p$q\"\n",
  "/bin/dash -c 'grep certificate /tmp/badhost.err || { p=__SPORE; q=_SHELL_CHECK_FAIL_TLS_HOSTNAME_REASON__; echo "
  "\"$p$q\"; }'\n",
  "confine net:none curl http://example.test:8080/\n",
  "confine net:tcp:10.0.2.2:8080 curl http://example.test:8080/\n",
  "confine net:dns nslookup -type=A example.com || echo __SPORE\"\"_SHELL_CHECK_FAIL_DNS__\n",
  "myc list-timers\n",
  "/bin/dash -c 'echo dash-ok; echo $((1 + 2)); cd /tmp && pwd'\n",
  "/bin/dash\n",
  "ls\n",
  "exit\n",
  "msh -c 'echo msh-c-ok'\n",
  "alias match='grep -x'\n",
  "echo 'two words' | match 'two words' || echo __SPORE\"\"_SHELL_CHECK_FAIL_ALIAS_PIPE__\n",
  "unalias match || echo __SPORE\"\"_SHELL_CHECK_FAIL_UNALIAS__\n",
  "command -v match && echo __SPORE\"\"_SHELL_CHECK_FAIL_ALIAS_REMAINS__\n",
  "__spore_harness_msh_paste__\n",
  "wc /tmp/msh-paste\n",
  "grep msh-paste-064 /tmp/msh-paste\n",
  "echo '#!/usr/bin/env msh' > /tmp/script.msh\n",
  "echo 'if true; then' >> /tmp/script.msh\n",
  "echo ' echo if-ok' >> /tmp/script.msh\n",
  "echo 'else' >> /tmp/script.msh\n",
  "echo ' echo if-bad' >> /tmp/script.msh\n",
  "echo 'fi' >> /tmp/script.msh\n",
  "echo 'while false; do' >> /tmp/script.msh\n",
  "echo ' echo loop-bad' >> /tmp/script.msh\n",
  "echo 'done' >> /tmp/script.msh\n",
  "echo 'echo script-ok' >> /tmp/script.msh\n",
  "chmod 755 /tmp/script.msh\n",
  "/tmp/script.msh\n",
  "echo one > /tmp/coreutils\n",
  "echo two >> /tmp/coreutils\n",
  "head -n 1 /tmp/coreutils\n",
  "tail -n 1 /tmp/coreutils\n",
  "wc /tmp/coreutils\n",
  "grep -n two /tmp/coreutils\n",
  "find /etc\n",
  "echo b > /tmp/sortin && echo a >> /tmp/sortin && sort /tmp/sortin\n",
  "echo a > /tmp/uniqin && echo a >> /tmp/uniqin && uniq -c /tmp/uniqin\n",
  "echo aa:bb > /tmp/cutin && cut -d : -f 2 /tmp/cutin\n",
  "echo abc > /tmp/trin && tr a-z A-Z < /tmp/trin\n",
  "echo hello | tr a-z A-Z | cat\n",
  "printf 'left\\nright\\n' | grep right | wc\n",
  "mkfifo /run/p\n",
  "cat /run/p &\n",
  "echo via-fifo > /run/p\n",
  "wait\n",
  "ls -l /run\n",
  "/libexec/spore-test/uds-server /run/echo.sock &\n",
  "/bin/dash -c 'for attempt in 1 2 3 4 5; do /libexec/spore-test/uds-client /run/echo.sock ping && exit 0; "
  "sleep 1; done; echo __SPORE\"\"_SHELL_CHECK_FAIL_UDS__'\n",
  "wait\n",
  "ls -l /run\n",
  "echo one two > /tmp/sedin && sed-lite 's/two/three/' /tmp/sedin\n",
  "tar -cf /tmp/coreutils.tar /tmp/coreutils\n",
  "tar -tf /tmp/coreutils.tar\n",
  "ps\n",
  "top -b -n 1\n",
  "sleep 1 &\n",
  "jobs\n",
  "cat /proc/1/status\n",
  "cat /proc/1/cmdline\n",
  "cat /proc/1/exe\n",
  "wait\n",
  "jobs\n",
  "/home/spore/demos/signal-crash all\n",
  "/home/spore/demos/signal-crash sleep &\n",
  "jobs\n",
  "kill -9 %1\n",
  "wait %1\n",
  "/bin/dash -c 'df -h | grep /tmp && "
  "case $(df -h --output=size / | tail -n 1) in *[KMGT]) true;; *) false;; esac && "
  "test $(df -m --output=size /proc | tail -n 1) -eq 0 || echo __SPORE\"\"_SHELL_CHECK_FAIL_DF__'\n",
  "free\n",
  "du -sh /tmp\n",
  "dirname /tmp/coreutils\n",
  "basename /tmp/coreutils\n",
  "date\n",
  "uptime\n",
  "whoami\n",
  "hostname\n",
  "hostid\n",
  "id\n",
  "who\n",
  "tty\n",
  "env FOO=bar printenv FOO\n",
  "printf 'abc\\n' > /tmp/hashin\n",
  "base64 /tmp/hashin > /tmp/hashin.b64\n",
  "base64 -d /tmp/hashin.b64\n",
  "cksum /tmp/hashin\n",
  "sum /tmp/hashin\n",
  "sha1sum /tmp/hashin\n",
  "du -s /tmp\n",
  "readlink -f /tmp/../tmp/hashin\n",
  "time true\n",
  "printf 'all:\\n\\techo mk-ok\\n' > /tmp/mkfile\n",
  "make -n -f /tmp/mkfile all | grep mk-ok || echo __SPORE\"\"_SHELL_CHECK_FAIL_MAKE__\n",
  "chroot / /bin/pwd\n",
  "cp /tmp/coreutils /tmp/coreutils.copy\n",
  "mv /tmp/coreutils.copy /tmp/coreutils.moved\n",
  "ln /tmp/coreutils /tmp/coreutils.link\n",
  "chmod 600 /tmp/coreutils\n",
  "stat /tmp/coreutils\n",
  "cat /tmp/coreutils.link\n",
  "tee /tmp/coreutils.tee < /tmp/coreutils\n",
  "cat /tmp/coreutils.tee\n",
  "printf '#!/usr/local/bin/jq -nef\\n\"spore\" | test(\"^spore$\")\\n' > /tmp/check-jq\n",
  "p=__SPORE; q=_SHELL_CHECK_FAIL_PACKAGES__; /bin/dash -c 'chmod 755 /tmp/check-jq && /tmp/check-jq && "
  "htop --version' || echo \"$p$q\"\n",
  "printf 'one\\ntwo\\n' | more\n",
  "printf 'one\\ntwo\\n' | less -F\n",
  "hexdump /tmp/coreutils\n",
  "xxd /tmp/coreutils\n",
  "sudo edit -p 'ed''-ready>' /etc/motd\n",
  "cat /etc/motd\n",
  "echo hi > /tmp/f\n",
  "cat /tmp/f\n",
  "edit /tmp/edit-test\na\nfrom edit\n.\nw\nq\n",
  "cat /tmp/edit-test\n",
  "edit -p 'ed''-ready>' /tmp/edit-paste\n",
  "wc /tmp/edit-paste\n",
  "grep edit-paste-064 /tmp/edit-paste\n",
  "pico /tmp/pico-test\n",
  "cat /tmp/pico-test\n",
  "pico /tmp/pico-paste\n",
  "wc /tmp/pico-paste\n",
  "grep pico-paste-032 /tmp/pico-paste\n",
  "nano /tmp/nano-paste\n",
  "wc /tmp/nano-paste\n",
  "grep nano-paste-032 /tmp/nano-paste\n",
  "ant\n",
  "mkdir /tmp/d && cd /tmp/d && touch x && ls\n",
  "/libexec/spore-test/pthread-demo\n",
  "ping -c 1 10.0.2.2\n",
  "/libexec/spore-test/udp-echo 10.0.2.2 5555 hi\n",
  "confine net:none /libexec/spore-test/udp-send 10.0.2.2 5555 hi\n",
  "confine net:udp:10.0.2.2:5555 /libexec/spore-test/udp-send 10.0.2.2 5555 hi\n",
  "confine compute-only /home/spore/demos/spinner\n",
  "confine fs:/tmp /bin/ls /tmp || echo __SPORE\"\"_SHELL_CHECK_FAIL_CONFINED_LS__\n",
  "confine fs:/tmp /bin/ls /etc/motd && echo __SPORE\"\"_SHELL_CHECK_FAIL_CONFINED_LS_ESCAPE__\n",
  "confine fs:/tmp /home/spore/demos/peeker /etc/motd\n",
  "confine fs:/tmp /home/spore/demos/writer /tmp/d/out\n",
  "confine mem:1 /home/spore/demos/memhog --expect-deny\n",
  "runc bad-manifest /home/spore/demos/escalate\n",
  "/bin/dash -c 'set -e; sudo -n mkuser sporecheck; test -d /home/sporecheck; "
  "test \"$(sudo -n -u sporecheck id -un)\" = sporecheck; "
  "if sudo -n -u sporecheck sudo -n id; then exit 1; fi; "
  "sudo -n passwd -S sporecheck; sudo -n deluser sporecheck; ! id sporecheck' "
  "|| echo __SPORE\"\"_SHELL_CHECK_FAIL_ACCOUNTS__\n",
  "sudo shutdown\n",
};

static size_t append_format(char *buf, size_t cap, size_t len, const char *fmt, ...) {
  if (len >= cap) { return len; }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
  if (n <= 0) { return len; }
  size_t add = (size_t)n;
  if (add > cap - len) { return cap; }
  return len + add;
}

static void send_numbered_lines(int fd, const char *prefix, const char *tag, size_t count, const char *suffix) {
  char input[16384];
  size_t len = 0;
  if (prefix != NULL) { len = append_format(input, sizeof(input), len, "%s", prefix); }
  for (size_t i = 1; i <= count; ++i) {
    len = append_format(input, sizeof(input), len, "%s-%03zu\n", tag, i);
  }
  if (suffix != NULL) { len = append_format(input, sizeof(input), len, "%s", suffix); }
  if (len > sizeof(input)) { len = sizeof(input); }
  write_serial_input(fd, input, len);
}

static void send_numbered_cr_lines(int fd, const char *tag, size_t count, const char *suffix) {
  char input[16384];
  size_t len = 0;
  for (size_t i = 1; i <= count; ++i) {
    len = append_format(input, sizeof(input), len, "%s-%03zu\r", tag, i);
  }
  if (suffix != NULL) { len = append_format(input, sizeof(input), len, "%s", suffix); }
  if (len > sizeof(input)) { len = sizeof(input); }
  write_serial_input(fd, input, len);
}

static void send_shell_printf_paste(int fd) {
  char input[4096];
  size_t len = append_format(input, sizeof(input), 0, "printf '");
  for (size_t i = 1; i <= 64; ++i) {
    len = append_format(input, sizeof(input), len, "msh-paste-%03zu\\n", i);
  }
  len = append_format(input, sizeof(input), len, "' > /tmp/msh-paste\n");
  if (len > sizeof(input)) { len = sizeof(input); }
  write_serial_input(fd, input, len);
}

bool shell_check_output(struct shell_check *state, int fd, char *buf, size_t *len) {
  if (strstr(buf, "__SPORE_SHELL_CHECK_FAIL_")) { return false; }
  if (state->input == SCRIPT_PICO_SIMPLE && strstr(buf, "GNU nano")) {
    static const char pico_input[] = "from pico\x0f\r\x18";
    write_serial_input(fd, pico_input, sizeof(pico_input) - 1);
    state->input = SCRIPT_NONE;
    state->nudge_pending = true;
    buf[0] = '\0';
    *len = 0;
  }
  if (state->input == SCRIPT_PICO_PASTE && strstr(buf, "GNU nano")) {
    send_numbered_cr_lines(fd, "pico-paste", 32, "\x0f\r\x18");
    state->input = SCRIPT_NONE;
    state->nudge_pending = true;
    buf[0] = '\0';
    *len = 0;
  }
  if (state->input == SCRIPT_NANO_PASTE && strstr(buf, "GNU nano")) {
    send_numbered_cr_lines(fd, "nano-paste", 32, "\x0f\r\x18");
    state->input = SCRIPT_NONE;
    state->nudge_pending = true;
    buf[0] = '\0';
    *len = 0;
  }
  if (state->input == SCRIPT_EDIT_MOTD && strstr(buf, "ed-ready>")) {
    static const char edit_input[] = "1d\na\nshell-check motd\n.\nw\nq\n";
    write_serial_input(fd, edit_input, sizeof(edit_input) - 1);
    state->input = SCRIPT_NONE;
    buf[0] = '\0';
    *len = 0;
  }
  if (state->input == SCRIPT_EDIT_PASTE && strstr(buf, "ed-ready>")) {
    send_numbered_lines(fd, "a\n", "edit-paste", 64, ".\nw\nq\n");
    state->input = SCRIPT_NONE;
    buf[0] = '\0';
    *len = 0;
  }
  ssize_t prompt = find_shell_prompt(buf, *len);
  if (state->sent < sizeof(shell_commands) / sizeof(shell_commands[0]) && prompt >= 0) {
    state->nudge_pending = false;
    if (state->input == SCRIPT_ANT_FIRST_INTERRUPT || state->input == SCRIPT_ANT_SECOND_INTERRUPT) { return false; }
    if (state->input == SCRIPT_ANT_EXIT) { state->input = SCRIPT_NONE; }
    if (strcmp(shell_commands[state->sent], "__spore_harness_msh_paste__\n") == 0) {
      send_shell_printf_paste(fd);
      ++state->sent;
      buf[0] = '\0';
      *len = 0;
      return true;
    }
    if (strcmp(shell_commands[state->sent], "sudo edit -p 'ed''-ready>' /etc/motd\n") == 0) { state->input = SCRIPT_EDIT_MOTD; }
    if (strcmp(shell_commands[state->sent], "edit -p 'ed''-ready>' /tmp/edit-paste\n") == 0) { state->input = SCRIPT_EDIT_PASTE; }
    if (strcmp(shell_commands[state->sent], "pico /tmp/pico-test\n") == 0) { state->input = SCRIPT_PICO_SIMPLE; }
    if (strcmp(shell_commands[state->sent], "pico /tmp/pico-paste\n") == 0) { state->input = SCRIPT_PICO_PASTE; }
    if (strcmp(shell_commands[state->sent], "nano /tmp/nano-paste\n") == 0) { state->input = SCRIPT_NANO_PASTE; }
    if (strcmp(shell_commands[state->sent], "ant\n") == 0) { state->input = SCRIPT_ANT_FIRST_INTERRUPT; }
    write_serial_input(fd, shell_commands[state->sent], strlen(shell_commands[state->sent]));
    ++state->sent;
    buf[0] = '\0';
    *len = 0;
  }
  return true;
}

void shell_check_idle(struct shell_check *state, int fd, char *buf, size_t *len) {
  if ((state->input == SCRIPT_ANT_FIRST_INTERRUPT || state->input == SCRIPT_ANT_SECOND_INTERRUPT) && strstr(buf, "❯") &&
      now_seconds() - state->last_output > 0.3 &&
      (state->input == SCRIPT_ANT_FIRST_INTERRUPT || strstr(buf, "press Ctrl+C again"))) {
    write_serial_input(fd, "\x03", 1);
    state->input = state->input == SCRIPT_ANT_FIRST_INTERRUPT ? SCRIPT_ANT_SECOND_INTERRUPT : SCRIPT_ANT_EXIT;
    buf[0] = 0;
    *len = 0;
  } else if (state->nudge_pending && now_seconds() - state->last_output > 1.0) {
    write_serial_input(fd, "\n", 1);
    state->nudge_pending = false;
  }
}
