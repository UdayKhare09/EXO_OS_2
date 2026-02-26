Plan: Comprehensive Networking Stack for EXO_OS
TL;DR — Build a full TCP/IP networking stack following the existing vtable + registry driver model (as used by blkdev_t). Two new source trees: src/kernel/drivers/net/ for NIC drivers (virtio-net first, E1000 second) and src/kernel/net/ for the protocol stack (Ethernet → ARP → IPv4 → ICMP → UDP → TCP) + socket layer + DHCP client. Extend file_t with a socket pointer so socket fds integrate with the existing fd table for mlibc POSIX compatibility. Add ~15 new syscalls (socket family 41-55 + poll + ioctl). No Makefile source changes needed (auto-discovery via find), only QEMU network flags.

Steps

Phase 0: Foundation — Kernel Infrastructure Prep
Extend file_t for non-vnode file descriptors in fd.h and fd.c: Add a void *private_data field and a file_ops_t *f_ops vtable pointer (with read, write, close, poll, ioctl function pointers). When f_ops is non-NULL, syscalls delegate to it instead of vnode->ops. This is the key mlibc prep — read()/write()/close() on socket fds "just work."

Add byte-order utilities in a new src/kernel/net/netutil.h: htons(), ntohs(), htonl(), ntohl() using __builtin_bswap16/32. Also add inet_checksum() (one's complement sum used by IP/ICMP/UDP/TCP).

Add a kernel timer facility in src/kernel/lib/timer.h and timer.c: A simple timer-wheel or sorted-list driven by the existing APIC tick (vector 0x20 in apic.c). Needed for TCP retransmission, ARP cache expiry, and DHCP lease timers. Callback-based: ktimer_add(ms, callback, arg) / ktimer_cancel(timer).

Add poll() syscall infrastructure — a new file src/kernel/syscall/poll_syscall.c: Implement syscall 7 (sys_poll). Define struct pollfd (POSIX-compatible: fd, events, revents). Each file_ops_t provides a poll method returning ready-event mask. For existing vnode-backed fds, a default poll can return POLLIN | POLLOUT (always ready). For sockets, poll checks RX buffer and connection state.

Add ioctl() syscall — extend syscall.c with syscall 16 (sys_ioctl). Dispatch to f_ops->ioctl if set, else to vnode's filesystem-specific handler. Needed for SIOCGIFADDR, SIOCSIFADDR, and socket options.

Phase 1: Network Device Abstraction Layer
Create src/kernel/drivers/net/netdev.h — Define netdev_t and netdev_ops_t following the blkdev_t pattern from blkdev.h:

netdev_ops_t: send_packet(dev, buf, len), get_mac(dev, mac_out), link_up(dev), set_rx_callback(dev, fn)
netdev_t: dev_id, name[16], mac[6], mtu (default 1500), netdev_ops_t *ops, void *priv, waitq_t rx_wq, ip_addr, netmask, gateway
Registry: netdev_register(dev) / netdev_get(id) / netdev_get_by_name(name) — static array g_netdevs[8]
Create src/kernel/drivers/net/netdev.c — Implement registration, packet-receive dispatch. The NIC ISR calls netdev_rx_dispatch(dev, packet, len) which pushes into a per-device RX ring buffer and wakes rx_wq.

Phase 2: NIC Drivers
Implement virtio-net driver in src/kernel/drivers/net/virtio_net.c and virtio_net.h:

Probe: pci_find(0x1AF4, 0x1000) (virtio transitional net) or device ID 0x1041 (virtio 1.0 net)
Follow the virtqueue setup pattern from virtio_blk.c: negotiate features, allocate RX/TX virtqueues via pmm_alloc_pages(), set up descriptor rings
Key virtio-net features: VIRTIO_NET_F_MAC, VIRTIO_NET_F_STATUS, VIRTIO_NET_F_MRG_RXBUF
RX: Pre-populate RX virtqueue with buffers (1526 bytes each = 14 eth + 1500 MTU + 12 virtio header). ISR on packet arrival → netdev_rx_dispatch()
TX: send_packet() posts to TX virtqueue, polls for completion (or interrupt-notified)
Register ISR via idt_register_handler(vector, virtio_net_isr) — use pci_find_cap(dev, 0x11) for MSI-X or fall back to legacy IRQ from dev->irq_line
Implement E1000 driver (second pass) in src/kernel/drivers/net/e1000.c and e1000.h:

Probe: pci_find(0x8086, 0x100E) (82540EM, QEMU default)
MMIO BAR0 via vmm_mmio_map(bar0_phys, bar0_size)
Init: reset (set CTRL.RST), configure RX/TX descriptor rings (256 entries each, page-aligned DMA), set RCTL (receive control) and TCTL (transmit control) registers
RX/TX descriptor rings: pmm_alloc_pages() for ring + buffer memory
ISR: register via idt_register_handler(), handle ICR register bits (RXT0, TXDW, LSC)
Register as netdev_t
Phase 3: Protocol Stack (src/kernel/net/)
Packet buffer abstraction — src/kernel/net/skbuff.h and skbuff.c: A skbuff_t (similar to Linux sk_buff) with data, len, head, tail, protocol, netdev, headroom/tailroom for header push/pull. Allocator: skb_alloc(size) via kmalloc(), skb_free(), skb_push(skb, len), skb_pull(skb, len).

Ethernet layer — src/kernel/net/ethernet.h and ethernet.c: Parse/build Ethernet frames. eth_rx(netdev, skb) — demux on EtherType: 0x0800 → ip_rx(), 0x0806 → arp_rx(). eth_tx(netdev, skb, dst_mac, ethertype) — prepend Ethernet header, call netdev->ops->send_packet().

ARP layer — src/kernel/net/arp.h and arp.c: ARP cache (hash table, 64 entries, 5-min expiry using kernel timer). arp_resolve(netdev, ip, mac_out) — returns cached MAC or sends ARP request and blocks on waitq_t until reply. arp_rx() — handles ARP requests (reply if target is our IP) and replies (update cache, wake waiters). Gratuitous ARP on interface up.

IPv4 layer — src/kernel/net/ipv4.h and ipv4.c: ip_rx(skb) — validate header checksum, check destination IP, demux on protocol: 1 → icmp_rx(), 6 → tcp_rx(), 17 → udp_rx(). ip_tx(skb, src, dst, protocol) — build IP header (TTL=64, compute checksum), ARP-resolve destination (or gateway if off-subnet), call eth_tx(). Simple routing: if (dst & netmask) == (src & netmask) → direct, else → gateway.

ICMP layer — src/kernel/net/icmp.h and icmp.c: Handle Echo Request → send Echo Reply (ping responder). icmp_send_echo_request() for user-space ping utility. Checksum validation.

UDP layer — src/kernel/net/udp.h and udp.c: udp_rx(skb) — validate checksum (optional for IPv4), lookup socket by (dst_port, dst_ip) in a global socket table, enqueue skb into socket's RX queue, wake waitq. udp_tx(socket, buf, len, dst_ip, dst_port) — build UDP header, call ip_tx(). Stateless — no connection tracking.

TCP layer — src/kernel/net/tcp.h and tcp.c (+ tcp_input.c, tcp_output.c, tcp_timer.c):

TCP Control Block (TCB): Per-connection state — snd_una, snd_nxt, rcv_nxt, rcv_wnd, snd_wnd, state (CLOSED→LISTEN→SYN_SENT→SYN_RCVD→ESTABLISHED→FIN_WAIT_1→...→TIME_WAIT), retransmit_queue, rx_buffer (ring buffer), waitq_t for each blocking event
State machine: Full RFC 793 state transitions — 3-way handshake (SYN/SYN-ACK/ACK), data transfer, 4-way close (FIN/ACK/FIN/ACK), RST handling
Input (tcp_input.c): Segment processing — sequence number validation, ACK processing (advance snd_una, free retransmit queue entries), data delivery to RX buffer, FIN/RST handling
Output (tcp_output.c): tcp_send(tcb, buf, len) — segment data, compute TCP checksum (pseudo-header), set sequence numbers, call ip_tx(). Window management: send up to min(snd_wnd, cwnd) bytes
Timers (tcp_timer.c): Retransmission timer (RTO, exponential backoff), TIME_WAIT timer (2×MSL), keepalive timer — all using the kernel timer facility from step 3
Congestion control: Start with simple slow-start + congestion avoidance (RFC 5681). cwnd, ssthresh, increase on ACK, halve on loss
Listening sockets: Accept queue (backlog) of completed connections; SYN_RCVD state for half-open connections
Phase 4: Socket Layer
Socket abstraction — src/kernel/net/socket.h and socket.c:

socket_t: domain (AF_INET), type (SOCK_STREAM/SOCK_DGRAM), protocol, state, local_addr (ip+port), remote_addr, rx_queue (skbuff list), tx_queue, rx_waitq, tx_waitq, accept_waitq, backlog, options (SO_REUSEADDR, etc.), tcp_tcb * (for SOCK_STREAM), file_ops_t vtable
socket_file_ops: socket_read(), socket_write(), socket_close(), socket_poll(), socket_ioctl() — these are what file_t.f_ops points to
Global socket lookup table for incoming packet demux: hash on (protocol, local_port), linked list for SO_REUSEADDR
Socket syscalls — src/kernel/syscall/net_syscalls.c:

sys_socket(domain, type, protocol) — allocate socket_t, allocate fd via fd_alloc(), set file_t.f_ops = &socket_file_ops, set file_t.private_data = socket
sys_bind(fd, addr, addrlen) — parse struct sockaddr_in, set local address, add to socket table
sys_listen(fd, backlog) — TCP only: set socket state to LISTENING, allocate accept queue
sys_accept(fd, addr, addrlen) — block on accept_waitq until connection in accept queue, create new socket + fd for the connection
sys_connect(fd, addr, addrlen) — UDP: set remote addr; TCP: initiate 3-way handshake, block until ESTABLISHED or error
sys_sendto(fd, buf, len, flags, dest_addr, addrlen) — copy from user buffer to skbuff, dispatch via UDP/TCP
sys_recvfrom(fd, buf, len, flags, src_addr, addrlen) — block on rx_waitq (or return EAGAIN if O_NONBLOCK), dequeue skbuff, copy to user buffer
sys_setsockopt/getsockopt(fd, level, optname, optval, optlen) — SO_REUSEADDR, SO_RCVBUF, TCP_NODELAY, etc.
sys_shutdown(fd, how), sys_getpeername(fd, ...), sys_getsockname(fd, ...)
Register all in g_syscall_table[] at Linux syscall numbers (41-55)
Phase 5: DHCP Client
DHCP client — src/kernel/net/dhcp.h and dhcp.c:
Implements DHCP DORA (Discover → Offer → Request → Ack) over UDP port 67/68
Runs as a kernel task spawned during net_init_task()
Parses DHCP options: IP address (option 50), subnet mask (option 1), gateway (option 3), DNS (option 6), lease time (option 51)
Configures netdev->ip_addr, netdev->netmask, netdev->gateway
Lease renewal timer via kernel timer
Fallback to static IP if DHCP fails after timeout
Phase 6: Integration & Wiring
Network init task — Add net_init_task() in src/kernel/net/net_init.c, spawned from kmain() in main.c after storage_init_task:

Initialize protocol stack (ethernet, ARP cache, IP, ICMP, UDP, TCP)
Probe and initialize NIC drivers (virtio_net_init, e1000_init)
Start DHCP client per interface
Log network status via klog()
QEMU network flags — Update the run target in makefile: Add -device virtio-net-pci,netdev=net0 -netdev user,id=net0,hostfwd=tcp::8080-:80 (user-mode networking with port forward for testing). For E1000 testing: -device e1000,netdev=net0.

Shell commands — Extend shell.c with network debugging commands: ifconfig (show interface IP/MAC/status), ping <ip> (ICMP echo), arp (show ARP cache), netstat (show open sockets/TCP connections).

Phase 7: mlibc Prep Headers
POSIX-compatible struct definitions — Ensure all user-visible structs match POSIX/Linux ABI expected by mlibc: struct sockaddr_in, struct sockaddr, struct in_addr, struct pollfd, sa_family_t, AF_INET=2, SOCK_STREAM=1, SOCK_DGRAM=2, IPPROTO_TCP=6, IPPROTO_UDP=17, SOL_SOCKET=1, etc. Define these in src/kernel/net/socket_defs.h so mlibc sysdep headers can mirror them.
New File Structure

Verification

Unit test: Ping — Boot with QEMU user-mode networking, run ping 10.0.2.2 (QEMU gateway) from shell. Expect ICMP Echo Reply.
Unit test: UDP — Send/receive UDP packets to QEMU host via port forward. A simple host-side Python socket.recvfrom() script verifies traffic.
Unit test: TCP — Connect to a host-side TCP server via port forward. Verify 3-way handshake, data transfer, and clean close.
Unit test: DHCP — Boot with -netdev user and verify automatic IP assignment (10.0.2.15 default from QEMU's DHCP).
Regression: Existing syscalls — Ensure read/write/close/open on regular files still work after file_t extension.
Socket fd test — Open a socket, call read()/write() on it, verify data flows through f_ops.
Decisions

NIC drivers: virtio-net first (matches existing virtio_blk pattern), E1000 second
file_t extension: Add f_ops + private_data (Approach C from research) — minimal VFS disruption, clean socket/fd integration, best mlibc path
TCP split: Separate tcp_input.c/tcp_output.c/tcp_timer.c to keep TCP manageable (~2000+ lines otherwise)
DHCP: Full DORA client with lease renewal — runs as kernel task
Syscall numbers: Linux x86-64 ABI (matches existing convention and mlibc expectations)
Implementation order: Foundation (Phase 0) → netdev (1) → virtio-net (2) → protocol stack bottom-up (3) → sockets (4) → DHCP (5) → integration (6) → E1000 (2b) → mlibc headers (7)