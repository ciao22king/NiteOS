#include <net.h>
#include <cpu.h>
#include <console.h>
#include <string.h>

struct eth_hdr {
    u8  dst[ETH_ALEN];
    u8  src[ETH_ALEN];
    u16 type;
} __attribute__((packed));

struct ip_hdr {
    u8  ihl_version;
    u8  tos;
    u16 total_len;
    u16 id;
    u16 frag_off;
    u8  ttl;
    u8  proto;
    u16 checksum;
    u32 src;
    u32 dst;
} __attribute__((packed));

struct udp_hdr {
    u16 src_port;
    u16 dst_port;
    u16 length;
    u16 checksum;
} __attribute__((packed));

struct icmp_hdr {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
} __attribute__((packed));

struct arp_entry { u32 ip; u8 mac[ETH_ALEN]; int valid; u64 stamp; };

static struct netif iface;
static const struct nic_ops *nic;
static struct arp_entry arp_table[ARP_TABLE_SIZE];
static u8 bcast_mac[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static const u8 zero_mac[ETH_ALEN] = {0};

static u32 g_icmp_reply_seen;
static u16 g_icmp_ident;

u32 ip_make(u8 a, u8 b, u8 c, u8 d) { return ((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | d; }

char *ip_str(u32 ip, char *buf)
{
    snprintf(buf, 16, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

u32 ip_parse(const char *s)
{
    u32 parts[4] = {0, 0, 0, 0};
    int idx = 0;
    while (*s && idx < 4) {
        u32 v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (u32)(*s - '0'); s++; }
        parts[idx++] = v & 0xFF;
        if (*s == '.') s++;
    }
    return ip_make((u8)parts[0], (u8)parts[1], (u8)parts[2], (u8)parts[3]);
}

int ip_same_subnet(u32 a, u32 b, u32 mask) { return (a & mask) == (b & mask); }

void net_register_nic(const struct nic_ops *ops) { nic = ops; }
int  net_ready(void) { return iface.present && iface.configured; }

int net_probe(void)
{
    const struct nic_ops *candidates[] = { e1000_driver(), NULL };
    for (int i = 0; candidates[i]; i++) {
        if (candidates[i]->probe && candidates[i]->probe()) {
            nic = candidates[i];
            return 0;
        }
    }
    return -1;
}
struct netif *net_iface(void) { return &iface; }

static u16 ip_checksum(const void *data, u32 len)
{
    const u8 *p = data;
    u32 sum = 0;
    while (len > 1) { sum += (u32)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (u32)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static void arp_insert(u32 ip, const u8 *mac)
{
    int free_slot = -1;
    u64 oldest = ~0ULL;
    int oldest_slot = 0;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            memcpy(arp_table[i].mac, mac, ETH_ALEN);
            arp_table[i].stamp = irq_get_ticks();
            return;
        }
        if (!arp_table[i].valid && free_slot < 0) free_slot = i;
        if (arp_table[i].stamp < oldest) { oldest = arp_table[i].stamp; oldest_slot = i; }
    }
    int slot = free_slot >= 0 ? free_slot : oldest_slot;
    arp_table[slot].ip = ip;
    memcpy(arp_table[slot].mac, mac, ETH_ALEN);
    arp_table[slot].valid = 1;
    arp_table[slot].stamp = irq_get_ticks();
}

static int arp_lookup(u32 ip, u8 *mac_out)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            if (mac_out) memcpy(mac_out, arp_table[i].mac, ETH_ALEN);
            return 1;
        }
    }
    return 0;
}

static int net_send_frame(const void *frame, u32 len)
{
    if (!nic || !iface.present) return -1;
    int r = nic->send(frame, len);
    if (r > 0) { iface.tx_packets++; iface.tx_bytes += (u64)r; }
    return r;
}

static void send_arp_request(u32 target_ip)
{
    struct {
        struct eth_hdr eth;
        struct { u16 hw_type, proto; u8 hw_len, proto_len; u16 op;
                 u8 sha[ETH_ALEN]; u32 spa; u8 tha[ETH_ALEN]; u32 tpa; } __attribute__((packed)) arp;
    } __attribute__((packed)) pkt;

    memcpy(pkt.eth.dst, bcast_mac, ETH_ALEN);
    memcpy(pkt.eth.src, iface.mac, ETH_ALEN);
    pkt.eth.type = htons16(ETH_TYPE_ARP);
    pkt.arp.hw_type = htons16(1);
    pkt.arp.proto = htons16(ETH_TYPE_IP);
    pkt.arp.hw_len = ETH_ALEN;
    pkt.arp.proto_len = 4;
    pkt.arp.op = htons16(1);
    memcpy(pkt.arp.sha, iface.mac, ETH_ALEN);
    pkt.arp.spa = htonl32(iface.ip);
    memcpy(pkt.arp.tha, zero_mac, ETH_ALEN);
    pkt.arp.tpa = htonl32(target_ip);

    net_send_frame(&pkt, sizeof(pkt));
}

int net_arp_resolve(u32 ip, u8 *mac_out, u32 timeout_ms)
{
    if (arp_lookup(ip, mac_out)) return 0;
    u64 deadline = irq_get_ticks() + timeout_ms / 10;
    send_arp_request(ip);
    u32 retries = 0;
    while (irq_get_ticks() < deadline) {
        net_poll();
        if (arp_lookup(ip, mac_out)) return 0;
        if (++retries % 100 == 0 && retries < 400) send_arp_request(ip);
    }
    return -1;
}

int net_send_ip(u32 dst_ip, u8 proto, const void *payload, u32 len)
{
    if (!iface.present) return -1;

    u8 mac[ETH_ALEN];
    int broadcast = (dst_ip == 0xFFFFFFFFu) ||
                    (iface.ip != 0 && dst_ip == (iface.ip | ~iface.netmask));

    if (broadcast) {
        memcpy(mac, bcast_mac, ETH_ALEN);
    } else {
        u32 next_hop = dst_ip;
        if (!ip_same_subnet(dst_ip, iface.ip, iface.netmask))
            next_hop = iface.gateway;
        if (net_arp_resolve(next_hop, mac, 2000) != 0) return -1;
    }

    u8 frame[NET_MAX_FRAME];
    struct eth_hdr *eth = (struct eth_hdr *)frame;
    struct ip_hdr *ip = (struct ip_hdr *)(frame + ETH_HDR_LEN);
    memcpy(eth->dst, mac, ETH_ALEN);
    memcpy(eth->src, iface.mac, ETH_ALEN);
    eth->type = htons16(ETH_TYPE_IP);

    memset(ip, 0, sizeof(*ip));
    ip->ihl_version = 0x45;
    ip->tos = 0;
    ip->total_len = htons16((u16)(sizeof(*ip) + len));
    ip->id = htons16((u16)(irq_get_ticks() & 0xFFFF));
    ip->frag_off = htons16(0x4000);   /* don't fragment */
    ip->ttl = 64;
    ip->proto = proto;
    ip->src = htonl32(iface.ip);
    ip->dst = htonl32(dst_ip);
    ip->checksum = 0;
    ip->checksum = htons16(ip_checksum(ip, sizeof(*ip)));

    if (sizeof(*ip) + len + ETH_HDR_LEN > NET_MAX_FRAME) return -1;
    memcpy(frame + ETH_HDR_LEN + sizeof(*ip), payload, len);

    return net_send_frame(frame, ETH_HDR_LEN + sizeof(*ip) + len);
}

int net_send_udp(u32 dst_ip, u16 src_port, u16 dst_port, const void *payload, u32 len)
{
    u8 buf[NET_MAX_FRAME - ETH_HDR_LEN - sizeof(struct ip_hdr)];
    struct udp_hdr *udp = (struct udp_hdr *)buf;
    if (sizeof(*udp) + len > sizeof(buf)) return -1;
    udp->src_port = htons16(src_port);
    udp->dst_port = htons16(dst_port);
    udp->length = htons16((u16)(sizeof(*udp) + len));
    udp->checksum = 0;    /* optional for IPv4 */
    memcpy(buf + sizeof(*udp), payload, len);
    return net_send_ip(dst_ip, IP_PROTO_UDP, buf, sizeof(*udp) + len);
}

int net_ping(u32 dst_ip)
{
    u8 buf[sizeof(struct icmp_hdr) + 32];
    struct icmp_hdr *icmp = (struct icmp_hdr *)buf;
    g_icmp_ident++;
    icmp->type = 8;     /* echo request */
    icmp->code = 0;
    icmp->id = htons16(g_icmp_ident);
    icmp->seq = htons16(1);
    for (int i = 0; i < 32; i++) buf[sizeof(struct icmp_hdr) + i] = (u8)i;
    icmp->checksum = 0;
    icmp->checksum = htons16(ip_checksum(buf, sizeof(buf)));

    u32 before = g_icmp_reply_seen;
    if (net_send_ip(dst_ip, IP_PROTO_ICMP, buf, sizeof(buf)) < 0) return -1;

    u64 deadline = irq_get_ticks() + 200;   /* 2 s at 100 Hz */
    while (irq_get_ticks() < deadline) {
        net_poll();
        if (g_icmp_reply_seen != before) return 0;
    }
    return -1;
}

/* ---------------- UDP dispatch ---------------- */

#define MAX_UDP_HANDLERS 8
static struct { u16 port; udp_handler_t handler; } udp_handlers[MAX_UDP_HANDLERS];
static int udp_handler_count;

static tcp_handler_t g_tcp_handler;
void tcp_set_handler(tcp_handler_t handler) { g_tcp_handler = handler; }

void udp_register_handler(u16 port, udp_handler_t handler)
{
    for (int i = 0; i < udp_handler_count; i++) {
        if (udp_handlers[i].port == port) { udp_handlers[i].handler = handler; return; }
    }
    if (udp_handler_count < MAX_UDP_HANDLERS) {
        udp_handlers[udp_handler_count].port = port;
        udp_handlers[udp_handler_count].handler = handler;
        udp_handler_count++;
    }
}

static void handle_arp(const u8 *payload, u32 len)
{
    if (len < 28) return;
    u16 op = ntohs16(*(const u16 *)(payload + 6));
    const u8 *sha = payload + 8;
    u32 spa = ntohl32(*(const u32 *)(payload + 14));
    u32 tpa = ntohl32(*(const u32 *)(payload + 24));

    arp_insert(spa, sha);

    if (op == 1 && (tpa == iface.ip || iface.ip == 0)) {
        /* Reply to the request. */
        u8 frame[64];
        struct eth_hdr *eth = (struct eth_hdr *)frame;
        u8 *arp = frame + ETH_HDR_LEN;
        memcpy(eth->dst, sha, ETH_ALEN);
        memcpy(eth->src, iface.mac, ETH_ALEN);
        eth->type = htons16(ETH_TYPE_ARP);
        *(u16 *)(arp + 0) = htons16(1);
        *(u16 *)(arp + 2) = htons16(ETH_TYPE_IP);
        arp[4] = ETH_ALEN; arp[5] = 4;
        *(u16 *)(arp + 6) = htons16(2);
        memcpy(arp + 8, iface.mac, ETH_ALEN);
        *(u32 *)(arp + 14) = htonl32(iface.ip);
        memcpy(arp + 18, sha, ETH_ALEN);
        *(u32 *)(arp + 24) = htonl32(spa);
        net_send_frame(frame, ETH_HDR_LEN + 28);
    }
}

static void handle_icmp(u32 src, const u8 *payload, u32 len)
{
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *icmp = (const struct icmp_hdr *)payload;
    if (icmp->type == 0) {          /* echo reply */
        if (ntohs16(icmp->id) == g_icmp_ident) g_icmp_reply_seen++;
    } else if (icmp->type == 8) {   /* echo request: reply */
        u8 reply[sizeof(struct icmp_hdr) + 64];
        u32 body = len - sizeof(struct icmp_hdr);
        if (body > 64) body = 64;
        memcpy(reply, payload, sizeof(struct icmp_hdr) + body);
        struct icmp_hdr *r = (struct icmp_hdr *)reply;
        r->type = 0;
        r->checksum = 0;
        r->checksum = htons16(ip_checksum(reply, sizeof(struct icmp_hdr) + body));
        net_send_ip(src, IP_PROTO_ICMP, reply, sizeof(struct icmp_hdr) + body);
    }
}

static void handle_udp(u32 src, const u8 *payload, u32 len)
{
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *udp = (const struct udp_hdr *)payload;
    u16 dst_port = ntohs16(udp->dst_port);
    u16 src_port = ntohs16(udp->src_port);
    u32 dlen = ntohs16(udp->length);
    if (dlen < sizeof(struct udp_hdr) || dlen > len) return;
    const u8 *data = payload + sizeof(struct udp_hdr);
    u32 data_len = dlen - sizeof(struct udp_hdr);

    for (int i = 0; i < udp_handler_count; i++) {
        if (udp_handlers[i].port == dst_port && udp_handlers[i].handler) {
            udp_handlers[i].handler(src, src_port, data, data_len);
            return;
        }
    }
}

static void handle_ip(const u8 *payload, u32 len)
{
    if (len < sizeof(struct ip_hdr)) return;
    const struct ip_hdr *ip = (const struct ip_hdr *)payload;
    u32 ihl = (ip->ihl_version & 0x0F) * 4;
    if (ihl < sizeof(struct ip_hdr) || ihl > len) return;

    u32 dst = ntohl32(ip->dst);
    if (dst != iface.ip && dst != 0xFFFFFFFFu && iface.ip != 0) return;

    u32 total = ntohs16(ip->total_len);
    if (total > len) total = len;
    const u8 *data = payload + ihl;
    u32 data_len = total > ihl ? total - ihl : 0;
    u32 src = ntohl32(ip->src);

    switch (ip->proto) {
    case IP_PROTO_ICMP: handle_icmp(src, data, data_len); break;
    case IP_PROTO_UDP:  handle_udp(src, data, data_len); break;
    case IP_PROTO_TCP:  if (g_tcp_handler) g_tcp_handler(src, data, data_len); break;
    default: break;
    }
}

static void handle_frame(const u8 *frame, u32 len)
{
    if (len < ETH_HDR_LEN) return;
    const struct eth_hdr *eth = (const struct eth_hdr *)frame;
    u16 type = ntohs16(eth->type);
    const u8 *payload = frame + ETH_HDR_LEN;
    u32 plen = len - ETH_HDR_LEN;

    if (type == ETH_TYPE_ARP) handle_arp(payload, plen);
    else if (type == ETH_TYPE_IP) handle_ip(payload, plen);
}

void net_poll(void)
{
    if (!nic || !iface.present) return;
    u8 frame[NET_MAX_FRAME];
    for (int i = 0; i < 32; i++) {
        int n = nic->recv(frame, sizeof(frame));
        if (n <= 0) break;
        iface.rx_packets++;
        iface.rx_bytes += (u64)n;
        handle_frame(frame, (u32)n);
    }
}

int net_init(void)
{
    memset(&iface, 0, sizeof(iface));
    memset(arp_table, 0, sizeof(arp_table));
    if (!nic) return -1;

    if (nic->init() != 0) {
        klog_warn("NIC driver '%s' failed to initialise", nic->name);
        return -1;
    }
    nic->get_mac(iface.mac);
    iface.present = 1;
    iface.link_up = nic->link_up();
    strlcpy(iface.driver, nic->name, sizeof(iface.driver));
    iface.ip = 0;
    iface.netmask = ip_make(255, 255, 255, 0);
    iface.gateway = 0;
    iface.dns = 0;

    klog_ok("NIC: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s",
            iface.driver, iface.mac[0], iface.mac[1], iface.mac[2],
            iface.mac[3], iface.mac[4], iface.mac[5],
            iface.link_up ? "up" : "down");
    return 0;
}

void net_print_status(void)
{
    if (!iface.present) { klog_warn("no network interface present"); return; }
    char ip[16], mask[16], gw[16], dns[16];
    klog_info("NET", "driver=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
              iface.driver, iface.mac[0], iface.mac[1], iface.mac[2],
              iface.mac[3], iface.mac[4], iface.mac[5]);
    klog_info("NET", "ip=%s mask=%s gateway=%s dns=%s",
              ip_str(iface.ip, ip), ip_str(iface.netmask, mask),
              ip_str(iface.gateway, gw), ip_str(iface.dns, dns));
    klog_info("NET", "rx=%u pkts / %u bytes, tx=%u pkts / %u bytes",
              (u32)iface.rx_packets, (u32)iface.rx_bytes,
              (u32)iface.tx_packets, (u32)iface.tx_bytes);
}
