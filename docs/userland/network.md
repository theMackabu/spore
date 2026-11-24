# Configuring the interactive network

Mycelium starts `hostname.service` to read `/etc/hostname`. Its `network.service` runs
`/bin/dhcpc` once. The client broadcasts a DHCP discovery, requests the offered address,
then applies the acknowledged address, mask, gateway and DNS server through Spore's
network configuration call. It writes the nameserver to `/etc/resolv.conf`.
[Network unit](../../userland/etc/mycelium/network.service) · [DHCP client](../../userland/bin/dhcpc/main.rs)

In the guest, `dhcpc --address` prints the configured IP and `myc status network.service`
shows the unit's state. App images receive their settings at build time.
[App settings](../host/app-settings.md) · [IPv4 routing](../net/ipv4.md)
