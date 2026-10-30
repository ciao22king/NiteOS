#include <net.h>
#include <tcp.h>
#include <http.h>
#include <cpu.h>
#include <console.h>
#include <string.h>

struct tcp_hdr {
    u16 src_port, dst_port;
    u32 seq, ack;
    u8  data_off_flags;      /* high nibble: header length in 32-bit words */
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_WINDOW 0x2000
#define MAX_SEGMENT 1400

struct pseudo_hdr {
    u32 src, dst;
    u8  zero, proto;
    u16 len;
} __attribute__((packed));

static struct {
    int  state;             /* 0 idle, 1 syn-sent, 2 established, 3 closing */
    u32  remote_ip;
    u16  local_port, remote_port;
    u32  snd_nxt, snd_una;
    u32  rcv_nxt;
    int  connected;
    int  got_reset;
    int  fin_received;
} conn;

int tcp_is_connected(void) { return conn.connected; }

static u16 tcp_checksum(u32 src, u32 dst, const u8 *seg, u32 len)
{
    u32 sum = 0;

    /* Pseudo-header, in network order: src(4) dst(4) zero(1) proto(1) len(2). */
    u8 ph[12];
    ph[0] = (u8)(src >> 24); ph[1] = (u8)(src >> 16);
    ph[2] = (u8)(src >> 8);  ph[3] = (u8)src;
    ph[4] = (u8)(dst >> 24); ph[5] = (u8)(dst >> 16);
    ph[6] = (u8)(dst >> 8);  ph[7] = (u8)dst;
    ph[8] = 0; ph[9] = IP_PROTO_TCP;
    ph[10] = (u8)(len >> 8); ph[11] = (u8)len;

    for (int i = 0; i < 12; i += 2)
        sum += (u32)((ph[i] << 8) | ph[i + 1]);

    u32 rem = len;
    while (rem > 1) { sum += (u32)((seg[0] << 8) | seg[1]); seg += 2; rem -= 2; }
    if (rem) sum += (u32)(seg[0] << 8);

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static void tcp_send_segment(u8 flags, const void *data, u32 len)
{
    u8 buf[NET_MAX_FRAME - ETH_HDR_LEN - IP_HDR_LEN];
    struct tcp_hdr *t = (struct tcp_hdr *)buf;
    memset(t, 0, sizeof(*t));
    t->src_port = htons16(conn.local_port);
    t->dst_port = htons16(conn.remote_port);
    t->seq = htonl32(conn.snd_nxt);
    t->ack = htonl32(conn.rcv_nxt);
    t->data_off_flags = (u8)((sizeof(struct tcp_hdr) / 4) << 4);
    t->flags = flags;
    t->window = htons16(TCP_WINDOW);
    if (len) memcpy(buf + sizeof(struct tcp_hdr), data, len);

    u32 seg_len = (u32)sizeof(struct tcp_hdr) + len;
    t->checksum = 0;
    t->checksum = htons16(tcp_checksum(net_iface()->ip, conn.remote_ip, buf, seg_len));
#ifdef TCP_DEBUG
    klog_info("TCPDBG", "tx flags=0x%02x seg_len=%u csum=0x%04x bytes %02x%02x%02x%02x %02x%02x%02x%02x ...",
              flags, seg_len, t->checksum,
              buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
#endif
    net_send_ip(conn.remote_ip, IP_PROTO_TCP, buf, seg_len);
}

static void tcp_handle(u32 src_ip, const u8 *payload, u32 len)
{
#ifdef TCP_DEBUG
    if (len >= sizeof(struct tcp_hdr)) {
        const struct tcp_hdr *dbg = (const struct tcp_hdr *)payload;
        klog_info("TCPDBG", "rx from %u.%u.%u.%u port %u->%u flags 0x%02x state=%d",
                  (src_ip>>24)&0xFF,(src_ip>>16)&0xFF,(src_ip>>8)&0xFF,src_ip&0xFF,
                  ntohs16(dbg->src_port), ntohs16(dbg->dst_port), dbg->flags, conn.state);
    }
#endif
    if (len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *t = (const struct tcp_hdr *)payload;
    if (ntohs16(t->dst_port) != conn.local_port) return;
    if (ntohs16(t->src_port) != conn.remote_port) return;

    u32 hdr_len = (t->data_off_flags >> 4) * 4;
    if (hdr_len < sizeof(struct tcp_hdr) || hdr_len > len) return;
    u32 data_len = len - hdr_len;
    u32 seq = ntohl32(t->seq);

    if (t->flags & TCP_RST) { conn.got_reset = 1; conn.state = 3; return; }

    if (conn.state == 1 && (t->flags & TCP_SYN)) {
        conn.rcv_nxt = seq + 1;
        conn.snd_una = conn.snd_nxt;      /* SYN was consumed when it was sent */
        conn.state = 2;
        conn.connected = 1;
        tcp_send_segment(TCP_ACK, NULL, 0);
        return;
    }

    if (conn.state == 2 || conn.state == 3) {
        if (data_len) {
            if (seq == conn.rcv_nxt) {
                conn.rcv_nxt += data_len;
                http_tcp_data(src_ip, payload + hdr_len, data_len);
            }
            tcp_send_segment(TCP_ACK, NULL, 0);
        }
        if (t->flags & TCP_FIN) {
            conn.rcv_nxt = seq + data_len + 1;
            tcp_send_segment(TCP_ACK, NULL, 0);
            conn.fin_received = 1;
            conn.state = 3;
        }
    }
}

int tcp_connect(u32 ip, u16 port, u32 timeout_ms)
{
    memset(&conn, 0, sizeof(conn));
    conn.remote_ip = ip;
    conn.remote_port = port;
    conn.local_port = (u16)(0xC000 + (rdtsc() & 0x3FFF));
    conn.snd_una = (u32)(rdtsc() & 0xFFFFFFFF);
    conn.snd_nxt = conn.snd_una;
    conn.state = 1;

    tcp_set_handler(tcp_handle);

#ifdef TCP_DEBUG
    klog_info("TCPDBG", "connecting to %u.%u.%u.%u:%u local_port=%u seq=0x%08x",
              (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF, port,
              conn.local_port, conn.snd_una);
#endif
    tcp_send_segment(TCP_SYN, NULL, 0);
    conn.snd_nxt++;   /* SYN consumes one sequence number */

    u64 deadline = irq_get_ticks() + timeout_ms / 10;
    u64 resend_at = irq_get_ticks() + 100;
    while (irq_get_ticks() < deadline) {
        net_poll();
        if (conn.connected) return 0;
        if (conn.got_reset) return -1;
        if (conn.state == 1 && irq_get_ticks() >= resend_at) {
            tcp_send_segment(TCP_SYN, NULL, 0);
            conn.snd_nxt++;
            resend_at = irq_get_ticks() + 100;
        }
    }
    return -1;
}

int tcp_send(const void *data, u32 len)
{
    if (!conn.connected) return -1;
    const u8 *p = data;
    u32 remaining = len;
    while (remaining) {
        u32 chunk = remaining > MAX_SEGMENT ? MAX_SEGMENT : remaining;
        tcp_send_segment(TCP_PSH | TCP_ACK, p, chunk);
        p += chunk;
        remaining -= chunk;
        conn.snd_nxt += chunk;

        u64 deadline = irq_get_ticks() + 50;
        while (irq_get_ticks() < deadline) {
            net_poll();
            if (conn.got_reset) return -1;
        }
    }
    return (int)len;
}

void tcp_close(void)
{
    if (conn.state == 2) {
        tcp_send_segment(TCP_FIN | TCP_ACK, NULL, 0);
        conn.snd_nxt++;
        u64 deadline = irq_get_ticks() + 100;
        while (irq_get_ticks() < deadline) net_poll();
    }
    conn.state = 0;
    conn.connected = 0;
}

void tcp_reset(void) { memset(&conn, 0, sizeof(conn)); }
