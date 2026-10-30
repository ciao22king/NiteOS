#include <net.h>
#include <cpu.h>
#include <console.h>
#include <string.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC 0x63825363u

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

struct dhcp_msg {
    u8  op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u32 ciaddr, yiaddr, siaddr, giaddr;
    u8  chaddr[16];
    u8  sname[64];
    u8  file[128];
    u32 magic;
    u8  options[312];
} __attribute__((packed));

static struct {
    u32 xid;
    u32 offered_ip, server_id, subnet, router, dns;
    int got_offer, got_ack;
} dhcp;

/* Copy the value of `code` into `out`; returns 1 if the option was present. */
static int dhcp_option(const u8 *opts, u32 len, u8 code, u8 *out, u32 out_len)
{
    u32 i = 0;
    while (i < len) {
        u8 c = opts[i];
        if (c == 0) { i++; continue; }        /* pad */
        if (c == 255) break;                  /* end */
        if (i + 1 >= len) break;
        u8 l = opts[i + 1];
        if (i + 2 + l > len) break;
        if (c == code) {
            u32 n = l < out_len ? l : out_len;
            memcpy(out, opts + i + 2, n);
            return 1;
        }
        i += 2 + l;
    }
    return 0;
}

static void dhcp_handler(u32 src_ip, u16 src_port, const void *data, u32 len)
{
    (void)src_ip; (void)src_port;
    if (len < sizeof(struct dhcp_msg) - sizeof(((struct dhcp_msg *)0)->options))
        return;
    const struct dhcp_msg *m = (const struct dhcp_msg *)data;
    if (m->xid != dhcp.xid) return;

    const u8 *opts = m->options;
    u32 opts_len = len - offsetof(struct dhcp_msg, options);
    u8 type = 0;
    dhcp_option(opts, opts_len, 53, &type, 1);

    if (type == DHCP_OFFER) {
        dhcp.offered_ip = ntohl32(m->yiaddr);
        u8 sid[4];
        if (dhcp_option(opts, opts_len, 54, sid, 4))
            dhcp.server_id = ip_make(sid[0], sid[1], sid[2], sid[3]);
        dhcp.got_offer = 1;
    } else if (type == DHCP_ACK) {
        dhcp.offered_ip = ntohl32(m->yiaddr);
        u8 v[4];
        if (dhcp_option(opts, opts_len, 1, v, 4)) dhcp.subnet = ip_make(v[0], v[1], v[2], v[3]);
        if (dhcp_option(opts, opts_len, 3, v, 4)) dhcp.router = ip_make(v[0], v[1], v[2], v[3]);
        if (dhcp_option(opts, opts_len, 6, v, 4)) dhcp.dns = ip_make(v[0], v[1], v[2], v[3]);
        dhcp.got_ack = 1;
    }
}

static void dhcp_send(u8 type)
{
    struct dhcp_msg m;
    memset(&m, 0, sizeof(m));
    m.op = 1;
    m.htype = 1;
    m.hlen = ETH_ALEN;
    m.xid = dhcp.xid;
    m.flags = htons16(0x8000);   /* broadcast reply requested */
    memcpy(m.chaddr, net_iface()->mac, ETH_ALEN);
    m.magic = htonl32(DHCP_MAGIC);

    u8 *o = m.options;
    int i = 0;
    o[i++] = 53; o[i++] = 1; o[i++] = type;
    if (type == DHCP_REQUEST) {
        o[i++] = 50; o[i++] = 4;
        o[i++] = (dhcp.offered_ip >> 24) & 0xFF; o[i++] = (dhcp.offered_ip >> 16) & 0xFF;
        o[i++] = (dhcp.offered_ip >> 8) & 0xFF;  o[i++] = dhcp.offered_ip & 0xFF;
        o[i++] = 54; o[i++] = 4;
        o[i++] = (dhcp.server_id >> 24) & 0xFF; o[i++] = (dhcp.server_id >> 16) & 0xFF;
        o[i++] = (dhcp.server_id >> 8) & 0xFF;  o[i++] = dhcp.server_id & 0xFF;
    }
    o[i++] = 55; o[i++] = 4; o[i++] = 1; o[i++] = 3; o[i++] = 6; o[i++] = 54;
    o[i++] = 255;

    net_send_udp(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &m, sizeof(m));
}

int dhcp_configure(u32 timeout_ms)
{
    struct netif *n = net_iface();
    if (!n->present) return -1;

    memset(&dhcp, 0, sizeof(dhcp));
    dhcp.xid = (u32)(rdtsc() ^ (rdtsc() >> 17)) | 0x5A5A0000u;
    dhcp.subnet = ip_make(255, 255, 255, 0);

    udp_register_handler(DHCP_CLIENT_PORT, dhcp_handler);

    u64 deadline = irq_get_ticks() + timeout_ms / 10;
    u64 next_send = 0;
    int phase = 0;   /* 0 = discover, 1 = request */

    while (irq_get_ticks() < deadline) {
        u64 now = irq_get_ticks();
        if (now >= next_send) {
            if (phase == 0) dhcp_send(DHCP_DISCOVER);
            else dhcp_send(DHCP_REQUEST);
            next_send = now + 100;   /* retry every second */
        }
        net_poll();

        if (phase == 0 && dhcp.got_offer) {
            phase = 1;
            next_send = 0;
        }
        if (dhcp.got_ack) {
            n->ip = dhcp.offered_ip;
            n->netmask = dhcp.subnet ? dhcp.subnet : ip_make(255, 255, 255, 0);
            n->gateway = dhcp.router;
            n->dns = dhcp.dns;
            n->dhcp_server = dhcp.server_id;
            n->configured = 1;
            return 0;
        }
    }
    return -1;
}
