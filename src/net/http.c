#include <http.h>
#include <net.h>
#include <tcp.h>
#include <cpu.h>
#include <console.h>
#include <string.h>

#define HDR_MAX 4096

static u8 *g_buf;
static u32 g_max, g_len;
static int g_headers_done;
static int g_status;
static u32 g_content_length;
static int g_have_content_length;
static int g_complete;
static char g_hdr[HDR_MAX];
static u32 g_hdrlen;

void http_reset(void)
{
    g_buf = NULL;
    g_max = g_len = 0;
    g_headers_done = 0;
    g_status = 0;
    g_content_length = 0;
    g_have_content_length = 0;
    g_complete = 0;
    g_hdrlen = 0;
}

int http_status_code(void) { return g_status; }

/* Parse the status line and the Content-Length header. */
static void parse_headers(void)
{
    char *line = g_hdr;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';
        if (str_starts_with(line, "HTTP/")) {
            const char *sp = strchr(line, ' ');
            if (sp) g_status = atoi(sp + 1);
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            const char *v = line + 15;
            while (*v == ' ' || *v == '\t') v++;
            g_content_length = (u32)atoi(v);
            g_have_content_length = 1;
        }
        if (!eol) break;
        line = eol + 1;
    }
}

static void append_body(const u8 *data, u32 len)
{
    if (g_len >= g_max) return;
    u32 n = len < (g_max - g_len) ? len : (g_max - g_len);
    memcpy(g_buf + g_len, data, n);
    g_len += n;
}

void http_tcp_data(u32 src_ip, const u8 *data, u32 len)
{
    (void)src_ip;
    if (g_complete || !g_buf) return;

    if (g_headers_done) {
        append_body(data, len);
    } else {
        u32 i = 0;
        for (; i < len; i++) {
            if (g_hdrlen < HDR_MAX - 1) g_hdr[g_hdrlen++] = (char)data[i];
            if (g_hdrlen >= 4 && g_hdr[g_hdrlen - 4] == '\r' && g_hdr[g_hdrlen - 3] == '\n' &&
                g_hdr[g_hdrlen - 2] == '\r' && g_hdr[g_hdrlen - 1] == '\n') {
                g_hdr[g_hdrlen] = '\0';
                parse_headers();
                g_headers_done = 1;
                if (i + 1 < len) append_body(data + i + 1, len - (i + 1));
                break;
            }
        }
    }

    if (g_headers_done && g_have_content_length && g_len >= g_content_length)
        g_complete = 1;
}

int http_get(u32 ip, u16 port, const char *host, const char *path,
             u8 *out, u32 max_out, u32 *out_len)
{
    http_reset();
    g_buf = out;
    g_max = max_out;

    char req[512];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: NiteKernel/1.0\r\n"
             "Connection: close\r\n\r\n",
             path, host);

    if (tcp_connect(ip, port, 4000) != 0) {
        klog_err("http: TCP connect to %u.%u.%u.%u:%u failed",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
        return -1;
    }
    tcp_send(req, strlen(req));

    /* Wait for the body, or for the peer to close the connection. */
    u64 deadline = irq_get_ticks() + 1500;   /* 15 s */
    while (irq_get_ticks() < deadline) {
        net_poll();
        if (g_complete) break;
        if (tcp_is_connected() == 0 && g_headers_done) break;
    }
    tcp_close();

    if (!g_headers_done || g_status < 200 || g_status >= 400) return -1;
    if (out_len) *out_len = g_len;
    return 0;
}
