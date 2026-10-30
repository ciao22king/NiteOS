#include <dns.h>
#include <net.h>
#include <cpu.h>
#include <console.h>
#include <string.h>

#define DNS_PORT 53
#define DNS_SOURCE_PORT 33333

struct dns_hdr {
    u16 id, flags, qdcount, ancount, nscount, arcount;
} __attribute__((packed));

static u32 override_server;
static u16 query_id;
static u32 resolved_ip;
static int have_answer;

static void dns_handler(u32 src_ip, u16 src_port, const void *data, u32 len)
{
    (void)src_ip; (void)src_port;
    if (len < sizeof(struct dns_hdr)) return;
    const u8 *p = data;
    const struct dns_hdr *h = (const struct dns_hdr *)p;
    if (ntohs16(h->id) != query_id) return;
    u16 ancount = ntohs16(h->ancount);
    u16 qdcount = ntohs16(h->qdcount);
    if (ancount == 0) return;

    /* Skip the question section. */
    u32 off = sizeof(struct dns_hdr);
    for (int q = 0; q < qdcount; q++) {
        while (off < len && p[off] != 0) {
            if ((p[off] & 0xC0) == 0xC0) { off += 2; break; }
            off += p[off] + 1;
        }
        if (off < len && p[off] == 0) off++;
        off += 4;   /* qtype + qclass */
    }

    for (int a = 0; a < ancount && off + 12 <= len; a++) {
        /* Name (possibly compressed). */
        if ((p[off] & 0xC0) == 0xC0) off += 2;
        else { while (off < len && p[off]) off += p[off] + 1; off++; }
        if (off + 10 > len) break;
        u16 type = (u16)((p[off] << 8) | p[off + 1]);
        u16 rdlen = (u16)((p[off + 8] << 8) | p[off + 9]);
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= len) {
            resolved_ip = ip_make(p[off], p[off + 1], p[off + 2], p[off + 3]);
            have_answer = 1;
            return;
        }
        off += rdlen;
    }
}

void dns_set_server(u32 server_ip) { override_server = server_ip; }

int dns_resolve_or_parse(const char *host, u32 *out_ip)
{
    int literal = 1;
    for (const char *c = host; *c; c++)
        if (!((*c >= '0' && *c <= '9') || *c == '.')) { literal = 0; break; }
    if (literal) { *out_ip = ip_parse(host); return 0; }
    return dns_resolve(host, out_ip, 5000);
}

int dns_resolve(const char *hostname, u32 *out_ip, u32 timeout_ms)
{
    u32 server = override_server ? override_server : net_iface()->dns;
    if (server == 0) return -1;

    u8 packet[512];
    memset(packet, 0, sizeof(packet));
    struct dns_hdr *h = (struct dns_hdr *)packet;
    query_id = (u16)(rdtsc() & 0xFFFF);
    h->id = htons16(query_id);
    h->flags = htons16(0x0100);   /* standard query, recursion desired */
    h->qdcount = htons16(1);

    /* Encode the hostname as length-prefixed labels. */
    u32 off = sizeof(struct dns_hdr);
    const char *label = hostname;
    while (*label && off < sizeof(packet) - 6) {
        const char *dot = label;
        while (*dot && *dot != '.') dot++;
        u32 llen = (u32)(dot - label);
        if (llen == 0 || llen > 63) break;
        packet[off++] = (u8)llen;
        memcpy(packet + off, label, llen);
        off += llen;
        if (*dot == '.') label = dot + 1;
        else break;
    }
    packet[off++] = 0;                 /* root label */
    packet[off++] = 0; packet[off++] = 1;   /* QTYPE = A */
    packet[off++] = 0; packet[off++] = 1;   /* QCLASS = IN */

    have_answer = 0;
    resolved_ip = 0;

    /* Query from a fixed high source port and listen for the reply there. */
    udp_register_handler(DNS_SOURCE_PORT, dns_handler);
    net_send_udp(server, DNS_SOURCE_PORT, DNS_PORT, packet, off);

    u64 deadline = irq_get_ticks() + timeout_ms / 10;
    while (irq_get_ticks() < deadline) {
        net_poll();
        if (have_answer) { if (out_ip) *out_ip = resolved_ip; return 0; }
    }
    return -1;
}
