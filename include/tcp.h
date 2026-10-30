#ifndef NEO_TCP_H
#define NEO_TCP_H

#include <nitekernel.h>

/* Minimal TCP client: one active connection at a time, which is all the
   driver-store download needs. */
int  tcp_connect(u32 ip, u16 port, u32 timeout_ms);
int  tcp_send(const void *data, u32 len);
void tcp_close(void);
int  tcp_is_connected(void);
void tcp_reset(void);

#endif
