#ifndef NEO_NET_H
#define NEO_NET_H

#include <nitekernel.h>

#define ETH_ALEN      6
#define ETH_HDR_LEN   14
#define ETH_MIN_FRAME 60
#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* Network byte order helpers (x86-64 is little-endian). */
static inline u16 htons16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs16(u16 v) { return htons16(v); }
static inline u32 htonl32(u32 v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
static inline u32 ntohl32(u32 v) { return htonl32(v); }

#define NET_MAX_FRAME 1536
#define ARP_TABLE_SIZE 16
#define IP_HDR_LEN    20      /* IPv4 header without options */

struct netif {
    int  present;
    u8   mac[ETH_ALEN];
    u32  ip, netmask, gateway, dns, dhcp_server;
    int  link_up;
    int  configured;             /* 1 once DHCP has assigned an address */
    char driver[24];
    u64  rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
};

/* A NIC driver implements this and calls net_register_nic(). */
struct nic_ops {
    const char *name;
    int  (*probe)(void);      /* 1 if matching hardware is present (no init) */
    int  (*init)(void);
    int  (*send)(const void *frame, u32 len);
    int  (*recv)(void *frame, u32 max);
    void (*get_mac)(u8 *mac);
    int  (*link_up)(void);
};

void net_register_nic(const struct nic_ops *ops);

/* NIC drivers available to the kernel; net_probe() picks the first that
   finds matching hardware. */
const struct nic_ops *e1000_driver(void);

/* Probe the PCI bus for a supported NIC and register it. Returns 0 on success. */
int  net_probe(void);

int  net_init(void);
int  net_ready(void);
struct netif *net_iface(void);
void net_poll(void);
void net_print_status(void);

/* Address helpers */
u32  ip_make(u8 a, u8 b, u8 c, u8 d);
char *ip_str(u32 ip, char *buf);
u32  ip_parse(const char *s);
int  ip_same_subnet(u32 a, u32 b, u32 mask);

/* Protocol helpers */
int  net_arp_resolve(u32 ip, u8 *mac_out, u32 timeout_ms);
int  net_send_ip(u32 dst_ip, u8 proto, const void *payload, u32 len);
int  net_send_udp(u32 dst_ip, u16 src_port, u16 dst_port, const void *payload, u32 len);
int  net_ping(u32 dst_ip);

/* DHCP client: fills in the interface address. Returns 0 on success. */
int  dhcp_configure(u32 timeout_ms);

/* UDP listener registration (used by the DHCP client). */
typedef void (*udp_handler_t)(u32 src_ip, u16 src_port, const void *data, u32 len);
void udp_register_handler(u16 port, udp_handler_t handler);

/* TCP segment dispatch (single active connection, used by the HTTP client). */
typedef void (*tcp_handler_t)(u32 src_ip, const u8 *segment, u32 len);
void tcp_set_handler(tcp_handler_t handler);

#endif
