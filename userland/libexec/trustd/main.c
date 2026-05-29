#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <mbedtls/error.h>
#include <mbedtls/platform.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

enum {
  MAX_HOST = 255,
  MAX_CERT = 65536,
  MAX_CERTS = 12,
};

static int read_full(int fd, void *buf, size_t len) {
  unsigned char *p = buf;
  while (len != 0) {
    ssize_t n = read(fd, p, len);
    if (n < 0 && errno == EINTR) { continue; }
    if (n <= 0) { return -1; }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static int write_full(int fd, const void *buf, size_t len) {
  const unsigned char *p = buf;
  while (len != 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0 && errno == EINTR) { continue; }
    if (n <= 0) { return -1; }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static void reply(int fd, int32_t rc, uint32_t flags) {
  uint32_t magic = 0x54525350u; /* PSRT */
  (void)write_full(fd, &magic, sizeof(magic));
  (void)write_full(fd, &rc, sizeof(rc));
  (void)write_full(fd, &flags, sizeof(flags));
}

static void handle_client(int fd, mbedtls_x509_crt *trust) {
  uint32_t magic = 0;
  uint32_t host_len = 0;
  uint32_t cert_count = 0;
  char host[MAX_HOST + 1];
  mbedtls_x509_crt chain;
  uint32_t flags = 0;
  int rc = -1;

  mbedtls_x509_crt_init(&chain);
  if (read_full(fd, &magic, sizeof(magic)) != 0 || magic != 0x31525453u) { goto out; } /* STR1 */
  if (read_full(fd, &host_len, sizeof(host_len)) != 0 || host_len == 0 || host_len > MAX_HOST) { goto out; }
  if (read_full(fd, host, host_len) != 0) { goto out; }
  host[host_len] = '\0';
  if (read_full(fd, &cert_count, sizeof(cert_count)) != 0 || cert_count == 0 || cert_count > MAX_CERTS) { goto out; }

  for (uint32_t i = 0; i < cert_count; ++i) {
    uint32_t cert_len = 0;
    unsigned char *cert = NULL;
    if (read_full(fd, &cert_len, sizeof(cert_len)) != 0 || cert_len == 0 || cert_len > MAX_CERT) { goto out; }
    cert = malloc(cert_len);
    if (cert == NULL) { goto out; }
    if (read_full(fd, cert, cert_len) != 0) {
      free(cert);
      goto out;
    }
    int parse = mbedtls_x509_crt_parse_der(&chain, cert, cert_len);
    free(cert);
    if (parse != 0) { goto out; }
  }

  rc =
    mbedtls_x509_crt_verify_with_profile(&chain, trust, NULL, &mbedtls_x509_crt_profile_next, host, &flags, NULL, NULL);

out:
  reply(fd, rc, flags);
  mbedtls_x509_crt_free(&chain);
}

int main(void) {
  if (psa_crypto_init() != PSA_SUCCESS) {
    fprintf(stderr, "trustd: psa crypto init failed\n");
    return 1;
  }
  const char *sock_path = "/run/trustd.sock";
  const char *ca_path = "/etc/ssl/certs/ca-certificates.crt";
  mbedtls_x509_crt trust;
  mbedtls_x509_crt_init(&trust);
  int rc = mbedtls_x509_crt_parse_file(&trust, ca_path);
  if (rc < 0) {
    char err[128];
    mbedtls_strerror(rc, err, sizeof(err));
    fprintf(stderr, "trustd: cannot load %s: %s\n", ca_path, err);
    return 1;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("trustd: socket");
    return 1;
  }
  unlink(sock_path);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("trustd: bind");
    return 1;
  }
  chmod(sock_path, 0666);
  if (listen(fd, 8) != 0) {
    perror("trustd: listen");
    return 1;
  }

  for (;;) {
    int client = accept(fd, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR) { continue; }
      continue;
    }
    handle_client(client, &trust);
    close(client);
  }
}
