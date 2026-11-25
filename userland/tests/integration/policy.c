#include "integration_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

constexpr int SYS_SPORE_APPLY_POLICY = 0x4005;

static void egress_child(const char *manifest, int expect_success) {
  if (syscall(SYS_SPORE_APPLY_POLICY, manifest) != 0) { integration_exit_group(20); }
  errno = 0;
  int rc = integration_udp_send_target(integration_ip4(10, 0, 2, 2), 5555);
  if (expect_success) { integration_exit_group(rc == 6 ? 0 : 21); }
  integration_exit_group(rc < 0 && errno == EPERM ? 0 : 22);
}

static void egress_attack_child(int mode) {
  long policy = 0;
  int rc = -1;
  switch (mode) {
  case 0:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
    errno = 0;
    rc = integration_udp_send_target(integration_ip4(10, 0, 2, 2), 5556);
    integration_exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 30);
  case 1:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.0/24:5555");
    rc = integration_udp_send_target(integration_ip4(10, 0, 2, 200), 5555);
    integration_exit_group(policy == 0 && rc == 6 ? 0 : 31);
  case 2:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.0/24:5555");
    errno = 0;
    rc = integration_udp_send_target(integration_ip4(10, 0, 3, 2), 5555);
    integration_exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 32);
  case 3:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
    errno = 0;
    rc = integration_udp_connect_send(integration_ip4(10, 0, 2, 2), 5555, 0, 0, 1);
    integration_exit_group(policy == 0 && rc == 5 ? 0 : 33);
  case 4:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
    errno = 0;
    rc = integration_udp_connect_send(integration_ip4(10, 0, 2, 2), 5555, integration_ip4(10, 0, 3, 2), 5555, 0);
    integration_exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 34);
  case 5: {
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa;
    integration_fill_udp_addr(&sa, integration_ip4(10, 0, 2, 2), 5556);
    errno = 0;
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    integration_exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 35);
  }
  default:
    integration_exit_group(99);
  }
}

static int run_egress_attack(int mode) {
  pid_t pid = fork();
  if (pid == 0) { egress_attack_child(mode); }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int egress_escalation_demo(void) {
  pid_t pid = fork();
  if (pid == 0) {
    if (syscall(SYS_SPORE_APPLY_POLICY, "net:none") != 0) { integration_exit_group(40); }
    pid_t grandchild = fork();
    if (grandchild == 0) {
      long rc = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
      integration_exit_group(rc < 0 && errno == EPERM ? 0 : 41);
    }
    int status = 0;
    waitpid(grandchild, &status, 0);
    integration_exit_group(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 42);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int check_egress_profile(const char *manifest, int expect_success) {
  pid_t child = fork();
  if (child == 0) { egress_child(manifest, expect_success); }
  int status;
  return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int integration_egress_demo(void) {
  int ok = check_egress_profile("net:none", 0) && check_egress_profile("net:udp:10.0.2.2:5555", 1) &&
           check_egress_profile("net:udp:10.0.2.7/29:5555", 1);
  int wrong_port = run_egress_attack(0);
  int cidr_in = run_egress_attack(1);
  int cidr_out = run_egress_attack(2);
  int connect_null = run_egress_attack(3);
  int connect_spoof = run_egress_attack(4);
  int connect_setpeer = run_egress_attack(5);
  int escalation = egress_escalation_demo();
  ok = ok && wrong_port && cidr_in && cidr_out && connect_null && connect_spoof && connect_setpeer && escalation;
  printf("[spore] v3f egress pressure: %s port=%s cidr-in=%s cidr-out=%s connect-null=%s connect-spoof=%s "
         "connect-setpeer=%s child-escalate=%s\n",
         ok ? "PASS" : "FAIL", wrong_port ? "PASS" : "FAIL", cidr_in ? "PASS" : "FAIL", cidr_out ? "PASS" : "FAIL",
         connect_null ? "PASS" : "FAIL", connect_spoof ? "PASS" : "FAIL", connect_setpeer ? "PASS" : "FAIL",
         escalation ? "PASS" : "FAIL");
  return ok;
}

static int check_filter(unsigned action) {
  pid_t child = fork();
  if (child == 0) {
    struct sock_filter code[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, action),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {.len = sizeof(code) / sizeof(code[0]), .filter = code};
    if (setuid(1000) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program)) {
      _exit(1);
    }
    if (action == SECCOMP_RET_KILL_PROCESS) {
      syscall(SYS_getppid);
      _exit(2);
    }
    pid_t next = fork();
    if (!next) {
      execl("/bin/exec_child", "exec_child", "filtered", (char *)NULL);
      _exit(3);
    }
    int status;
    _exit(next > 0 && waitpid(next, &status, 0) == next && status == 0 ? 0 : 4);
  }
  int status;
  if (child < 0 || waitpid(child, &status, 0) != child) { return 0; }
  return action == SECCOMP_RET_KILL_PROCESS ? WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS : status == 0;
}

int integration_seccomp(void) {
  int ok = check_filter(SECCOMP_RET_ERRNO | EACCES) && check_filter(SECCOMP_RET_KILL_PROCESS);
  printf("[spore] seccomp syscall denial, inheritance and termination: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_uts_namespace(void) {
  struct stat original;
  if (stat("/proc/self/ns/uts", &original)) { return 0; }
  char before[65], after[65];
  if (gethostname(before, sizeof(before))) { return 0; }
  pid_t child = syscall(SYS_clone, CLONE_NEWUTS | SIGCHLD, 0, 0, 0, 0);
  if (child == 0) {
    struct stat shared, copied;
    int nsfd = open("/proc/self/ns/uts", O_RDONLY | O_CLOEXEC);
    if (nsfd < 0 || fstat(nsfd, &copied) || copied.st_ino == original.st_ino || copied.st_dev != original.st_dev ||
        unshare(CLONE_NEWUTS) || stat("/proc/self/ns/uts", &shared) || shared.st_ino == copied.st_ino ||
        fstat(nsfd, &shared) || shared.st_ino != copied.st_ino) {
      _exit(7);
    }
    close(nsfd);
    if (gethostname(after, sizeof(after)) || strcmp(before, after) || sethostname("private", 7)) { _exit(1); }
    pid_t next = fork();
    if (!next) {
      if (gethostname(after, sizeof(after)) || strcmp(after, "private") || unshare(CLONE_NEWUTS) ||
          sethostname("nested", 6)) {
        _exit(2);
      }
      _exit(0);
    }
    int status;
    _exit(next > 0 && waitpid(next, &status, 0) == next && status == 0 && !gethostname(after, sizeof(after)) &&
              !strcmp(after, "private")
            ? 0
            : 3);
  }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0 && !gethostname(after, sizeof(after)) &&
           !strcmp(after, before);
  printf("[spore] private UTS hostname and fork inheritance: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_capabilities(void) {
  pid_t child = fork();
  if (!child) {
    struct __user_cap_header_struct hdr = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2];
    uint32_t bit = 1u << CAP_SYS_ADMIN;
    if (syscall(SYS_capget, &hdr, caps) || !(caps[0].effective & bit) ||
        prctl(PR_CAPBSET_DROP, CAP_SYS_ADMIN, 0, 0, 0) || prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0) != 0) {
      _exit(1);
    }
    caps[0].effective &= ~bit;
    caps[0].permitted &= ~bit;
    if (syscall(SYS_capset, &hdr, caps) || unshare(CLONE_NEWUTS) != -1 || errno != EPERM) { _exit(2); }
    caps[0].permitted |= bit;
    if (syscall(SYS_capset, &hdr, caps) != -1 || errno != EPERM) { _exit(3); }
    execl("/bin/exec_child", "exec_child", "cap-bound", (char *)NULL);
    _exit(4);
  }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0;
  printf("[spore] capability drop and exec bounding set: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int write_map(const char *path, const char *text) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) { return -1; }
  ssize_t rc = write(fd, text, strlen(text));
  int saved = errno;
  close(fd);
  errno = saved;
  return rc == (ssize_t)strlen(text) ? 0 : -1;
}

int integration_user_namespace(void) {
  pid_t child = fork();
  if (!child) {
    struct stat initial_ipc, private_ipc;
    if (stat("/proc/self/ns/ipc", &initial_ipc) || setgid(1000) || setuid(1000) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || unshare(CLONE_NEWIPC) != -1 || errno != EPERM ||
        unshare(CLONE_NEWUSER | CLONE_NEWIPC) || getuid() != 65534 || getgid() != 65534) {
      _exit(1);
    }
    if (stat("/proc/self/ns/ipc", &private_ipc) || initial_ipc.st_ino == private_ipc.st_ino) { _exit(7); }
    pid_t peer = fork();
    if (!peer) {
      struct stat shared;
      _exit(stat("/proc/self/ns/ipc", &shared) || shared.st_ino != private_ipc.st_ino || unshare(CLONE_NEWIPC) ||
            stat("/proc/self/ns/ipc", &shared) || shared.st_ino == private_ipc.st_ino);
    }
    int ipc_status;
    if (peer < 0 || waitpid(peer, &ipc_status, 0) != peer || ipc_status) { _exit(8); }
    if (write_map("/proc/self/uid_map", "0 1000 1\n") || write_map("/proc/self/setgroups", "deny\n") ||
        write_map("/proc/self/gid_map", "0 1000 1\n") || getuid() != 0 || getgid() != 0) {
      _exit(2);
    }
    if (sethostname("global", 6) != -1 || errno != EPERM || setuid(1000) != -1 || errno != EINVAL) { _exit(3); }
    if (mount("none", "/tmp", "tmpfs", 0, NULL) != -1 || errno != EPERM || unshare(CLONE_NEWUTS | CLONE_NEWNS) ||
        sethostname("child", 5) || mount("none", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, NULL) || umount("/tmp")) {
      _exit(4);
    }
    struct stat st;
    if (stat("/", &st) || st.st_uid != 65534) { _exit(5); }
    pid_t nested = fork();
    if (!nested) {
      if (unshare(CLONE_NEWUSER) || write_map("/proc/self/uid_map", "7 1900 1\n") != -1 ||
          write_map("/proc/self/uid_map", "7 0 1\n") || write_map("/proc/self/gid_map", "9 0 1\n") || getuid() != 7 ||
          getgid() != 9 || sethostname("ancestor", 8) == 0) {
        _exit(9);
      }
      _exit(0);
    }
    if (nested < 0 || waitpid(nested, &ipc_status, 0) != nested || ipc_status) { _exit(10); }
    execl("/bin/exec_child", "exec_child", "mapped-user", (char *)NULL);
    _exit(6);
  }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0;
  printf("[spore] private user and group mappings: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int tmpfs_bind(void) {
  const char *src = "/tmp/mount-test/src///", *dst = "/tmp/mount-test/bind";
  if (mkdir(src, 0700) || mkdir(dst, 0700) || mount(src, dst, NULL, MS_BIND | MS_REC, NULL)) { return 0; }
  if (rename(src, "/tmp/mount-test/moved")) { return 0; }
  int fd = open("/tmp/mount-test/bind/test", O_CREAT | O_EXCL | O_RDWR, 0600);
  struct stat original, alias;
  int ok = fd >= 0 && !fstat(fd, &alias) && !stat("/tmp/mount-test/moved/test", &original) &&
           original.st_dev == alias.st_dev && original.st_ino == alias.st_ino &&
           rename("/tmp/mount-test/bind/test", "/tmp/mount-test/moved/x") < 0 && errno == EXDEV;
  if (fd >= 0) { close(fd); }
  if (!ok || umount(dst) || !access("/tmp/mount-test/bind/test", F_OK) || errno != ENOENT) { return 0; }
  const char *sub = "/tmp/mount-test/moved/sub", *alias_sub = "/tmp/mount-test/bind/sub";
  if (mkdir(sub, 0700) || mount("none", sub, "tmpfs", 0, NULL) ||
      mount("/tmp/mount-test/moved", dst, NULL, MS_BIND | MS_REC, NULL)) {
    return 0;
  }
  return !stat(sub, &original) && !stat(alias_sub, &alias) && original.st_dev == alias.st_dev && !umount(alias_sub) &&
         !stat(alias_sub, &alias) && alias.st_dev != original.st_dev && !umount(dst) && !umount(sub);
}

static int private_ptys(void) {
  const char *a = "/tmp/mount-test/pts-a", *b = "/tmp/mount-test/pts-b";
  const char *opts = "newinstance,ptmxmode=0666,mode=0620";
  if (mkdir(a, 0755) || mkdir(b, 0755) || mount("devpts", a, "devpts", MS_NOSUID | MS_NOEXEC, opts) ||
      mount("devpts", b, "devpts", MS_NOSUID | MS_NOEXEC, opts)) {
    return 0;
  }
  if (symlink("pts-a/ptmx", "/tmp/mount-test/ptmx")) { return 0; }
  int ma = open("/tmp/mount-test/ptmx", O_RDWR), mb = open("/tmp/mount-test/pts-b/ptmx", O_RDWR);
  unsigned na = 1, nb = 1;
  int unlock = 0;
  if (ma < 0 || mb < 0 || ioctl(ma, TIOCGPTN, &na) || ioctl(mb, TIOCGPTN, &nb) || na || nb ||
      ioctl(ma, TIOCSPTLCK, &unlock) || ioctl(mb, TIOCSPTLCK, &unlock)) {
    return 0;
  }
  int sa = open("/tmp/mount-test/pts-a/0", O_RDWR | O_NONBLOCK);
  int sb = open("/tmp/mount-test/pts-b/0", O_RDWR | O_NONBLOCK);
  struct stat xa, xb;
  char bytes[2];
  int ok = sa >= 0 && sb >= 0 && !fstat(sa, &xa) && !fstat(sb, &xb) && xa.st_dev != xb.st_dev &&
           (xa.st_mode & 0777) == 0620 && write(ma, "a\n", 2) == 2 && read(sb, bytes, 2) == -1 && errno == EAGAIN &&
           read(sa, bytes, 2) == 2 && bytes[0] == 'a';
  close(ma);
  close(mb);
  ok &= access("/tmp/mount-test/pts-a/0", F_OK) == -1 && errno == ENOENT;
  if (sa >= 0) { close(sa); }
  if (sb >= 0) { close(sb); }
  return ok && !umount(a) && !umount(b);
}

static int isolated_tmpfs(int ready, int resume) {
  const char *dir = "/tmp/mount-test";
  const char *file = "/tmp/mount-test/contents";
  struct stat base, mounted;
  if (stat(dir, &base) || mount("none", dir, "tmpfs", MS_NODEV | MS_NOSUID, "mode=0710") || stat(dir, &mounted) ||
      base.st_dev == mounted.st_dev || (mounted.st_mode & 07777) != 0710) {
    return 1;
  }
  if (!tmpfs_bind() || !private_ptys()) { return 9; }
  int fd = open(file, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0 || write(fd, "private", 7) != 7) { return 2; }
  char *mapped = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) { return 8; }
  mapped[0] = 'P';
  sync();
  if (munmap(mapped, 4096)) { return 8; }
  char byte;
  if (write(ready, ".", 1) != 1 || read(resume, &byte, 1) != 1) { return 8; }
  pid_t nested = fork();
  if (nested == 0) {
    if (unshare(CLONE_NEWNS) || umount2(dir, MNT_DETACH) || stat(dir, &mounted) || mounted.st_dev != base.st_dev ||
        !access(file, F_OK)) {
      _exit(3);
    }
    char buf[7];
    _exit(pread(fd, buf, 7, 0) != 7 || memcmp(buf, "Private", 7) ? 4 : 0);
  }
  int status;
  if (nested < 0 || waitpid(nested, &status, 0) != nested || status || access(file, F_OK) || umount(dir) != -1 ||
      errno != EBUSY) {
    return 5;
  }
  close(fd);
  pid_t shared = fork();
  if (!shared) { _exit(umount(dir) ? 6 : 0); }
  if (shared < 0 || waitpid(shared, &status, 0) != shared || status || !access(file, F_OK)) { return 7; }
  return 0;
}

static int pivot_child(void) {
  const char *root = "/tmp/pivot-test";
  int proc = open("/proc", O_DIRECTORY | O_RDONLY);
  if (proc < 0 || mount("tmpfs", root, "tmpfs", 0, NULL) || chdir(root) || mkdir("previous", 0700) ||
      mkdir("copy", 0700)) {
    return 1;
  }
  if (syscall(SYS_pivot_root, ".", "previous")) { return 2; }
  char cwd[32];
  if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/") || !access("/etc/motd", F_OK)) { return 3; }
  int status = openat(proc, "self/status", O_RDONLY);
  if (status < 0) { return 4; }
  close(status);
  struct stat old, clone;
  if (stat("/previous/etc/motd", &old) || mount("/previous", "/copy", NULL, MS_BIND | MS_REC, NULL) ||
      stat("/copy/etc/motd", &clone) || clone.st_dev != old.st_dev || clone.st_ino != old.st_ino) {
    return 5;
  }
  int writer = open("/copy/etc/motd", O_WRONLY);
  unsigned long ro = MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV;
  if (writer < 0 || mount("none", "/copy", NULL, ro, NULL) == 0 || errno != EBUSY) { return 7; }
  close(writer);
  if (mount("none", "/copy", NULL, ro, NULL) || open("/copy/etc/motd", O_WRONLY) >= 0 || errno != EROFS ||
      mkdir("/copy/pivot-should-fail", 0700) == 0 || errno != EROFS) {
    return 8;
  }
  if (mkdir("/work", 0700) || mount("/previous/etc", "/work", NULL, MS_BIND, NULL)) { return 13; }
  int work = open("/work/pivot-probe", O_RDWR | O_CREAT | O_EXCL, 0600);
  if (work < 0 || write(work, "x", 1) != 1 || stat("/previous/etc/pivot-probe", &clone)) { return 14; }
  close(work);
  if (unlink("/work/pivot-probe") || !access("/previous/etc/pivot-probe", F_OK) || umount("/work")) { return 15; }
  if (symlink("/copy", "/previous/pivot-link") || stat("/previous/pivot-link/etc/motd", &clone) ||
      clone.st_ino != old.st_ino || unlink("/previous/pivot-link")) {
    return 9;
  }
  if (umount2("/previous", MNT_DETACH) || !access("/previous/etc/motd", F_OK) || access("/copy/etc/motd", F_OK)) {
    return 6;
  }
  int staging_root = open("/", O_DIRECTORY | O_RDONLY);
  if (staging_root < 0 || chdir("/copy") || syscall(SYS_pivot_root, ".", ".") || fchdir(staging_root) ||
      umount2(".", MNT_DETACH) || chdir("/")) {
    return 10;
  }
  close(staging_root);
  int descriptors = openat(proc, "self/fd", O_DIRECTORY | O_RDONLY);
  if (descriptors < 0) { return 12; }
  close(descriptors);
  close(proc);
  return stat("/etc/motd", &clone) || clone.st_ino != old.st_ino || open("/etc/motd", O_WRONLY) >= 0 || errno != EROFS
           ? 11
           : 0;
}

static int pivot_namespace(void) {
  if (mkdir("/tmp/pivot-test", 0700)) { return 0; }
  pid_t child = syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
  if (!child) { _exit(pivot_child()); }
  int status = -1;
  int ok = child > 0 && waitpid(child, &status, 0) == child && !status && !access("/etc/motd", F_OK);
  ok &= rmdir("/tmp/pivot-test") == 0;
  printf("[spore] private root pivot and inherited proc directory: %s status=%d\n", ok ? "PASS" : "FAIL", status);
  return ok;
}

static int shared_tmpfs(void) {
  const char *root = "/tmp/mount-test";
  if (mount("tmpfs", root, "tmpfs", 0, NULL) || mount(NULL, root, NULL, MS_SHARED, NULL) || chdir(root) ||
      mkdir("nested", 0700)) {
    return 1;
  }
  int pid = syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
  if (!pid) {
    if (mount("tmpfs", "nested", "tmpfs", 0, "size=4k")) { _exit(1); }
    int fd = open("nested/page", O_CREAT | O_WRONLY, 0600);
    static const char page[4096];
    _exit(fd < 0 || write(fd, page, sizeof(page)) != sizeof(page) || write(fd, "x", 1) != -1 || errno != ENOSPC);
  }
  int status;
  struct stat st;
  return pid < 0 || waitpid(pid, &status, 0) != pid || status || stat("nested/page", &st) || st.st_size != 4096;
}

int integration_mount_namespace(void) {
  const char *dir = "/tmp/mount-test";
  if (mkdir(dir, 0700)) { return 0; }
  int ready[2], resume[2];
  if (pipe(ready) || pipe(resume)) { return 0; }
  pid_t child = syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
  if (!child) {
    close(ready[0]);
    close(resume[1]);
    _exit(isolated_tmpfs(ready[1], resume[0]));
  }
  close(ready[1]);
  close(resume[0]);
  char byte;
  int status = -1;
  int ok = child > 0 && read(ready[0], &byte, 1) == 1;
  if (ok) {
    ok = access("/tmp/mount-test/contents", F_OK) == -1 && errno == ENOENT;
    char path[64], mounts[2048];
    snprintf(path, sizeof(path), "/proc/%d/mountinfo", child);
    int fd = open(path, O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, mounts, sizeof(mounts) - 1);
    if (fd >= 0) { close(fd); }
    if (n <= 0) {
      ok = 0;
    } else {
      mounts[n] = 0;
      ok &= strstr(mounts, " /tmp/mount-test rw,nosuid,nodev ") != NULL;
    }
    ok &= write(resume[1], ".", 1) == 1;
  }
  close(resume[1]);
  close(ready[0]);
  ok &= child > 0 && waitpid(child, &status, 0) == child && !status;
  pid_t shared = syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
  if (!shared) { _exit(shared_tmpfs()); }
  ok &= shared > 0 && waitpid(shared, &status, 0) == shared && !status;
  ok &= rmdir(dir) == 0;
  printf("[spore] private tmpfs and mount namespace inheritance: %s status=%d\n", ok ? "PASS" : "FAIL", status);
  return ok && pivot_namespace();
}
