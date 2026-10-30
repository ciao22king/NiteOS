#ifndef NEO_HTTP_H
#define NEO_HTTP_H

#include <nitekernel.h>

/* Called by the TCP layer when payload bytes arrive. */
void http_tcp_data(u32 src_ip, const u8 *data, u32 len);

/* Perform an HTTP/1.0 GET. Returns 0 on success (a 2xx response) and fills
   `out` with the response body. `out_len` receives the body length. */
int  http_get(u32 ip, u16 port, const char *host, const char *path,
              u8 *out, u32 max_out, u32 *out_len);

void http_reset(void);
int  http_status_code(void);

#endif
