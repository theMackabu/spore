# How curl verifies a server

The interactive image builds curl with an mbedTLS patch that can ask trustd to verify
the peer's certificate chain. Mycelium starts trustd as a service. At startup it parses
`/etc/ssl/certs/ca-certificates.crt` once and listens on `/run/trustd.sock`.
[Curl patch](../../userland/usr/local/bin/curl/patches/0001-spore-trustd-mbedtls.patch) · [Service unit](../../userland/etc/mycelium/trustd.service)

For each request, trustd reads the hostname and certificates, checks certificate dates
and verifies the chain and hostname against its cached trust anchors. It returns the
result and verification flags to the client. [Verifier](../../userland/libexec/trustd/main.c) · [Unix sockets](../net/unix.md)

[Package manager](packages.md) · [Guest network](network.md)
