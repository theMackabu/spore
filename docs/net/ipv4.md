# How an IPv4 packet reaches the wire

The kernel picks a next hop from the guest address, netmask and gateway. For Ethernet
traffic it looks up the next hop's MAC address in the ARP cache. On a miss it queues
the packet while it requests the address. A matching ARP reply releases queued packets
to the virtio network driver. [Routing and ARP](../../kernel/net/net.c) · [Virtio network driver](../../kernel/drivers/virtio_net.c)

Incoming frames are parsed and dispatched to ARP, ICMP, UDP or TCP handling. Loopback
traffic stays inside the guest. App images take their static configuration from the payload.
In the interactive image a [DHCP service](../userland/network.md) sets the address and route.
[Packet parsing](../../kernel/net/packet.rs) · [App networking](../host/app-settings.md)

[Internet sockets](sockets.md) · [TCP streams](tcp.md) · [Unix-domain sockets](unix.md)
