#ifndef NEO_DNS_H
#define NEO_DNS_H

#include <nitekernel.h>

/* Resolve a hostname to an IPv4 address using the interface's DNS server.
   Returns 0 on success and stores the address in *out_ip. */
int dns_resolve(const char *hostname, u32 *out_ip, u32 timeout_ms);

/* Set an explicit DNS server (overrides the DHCP-provided one). */
void dns_set_server(u32 server_ip);

/* Accept either a dotted-quad literal or a hostname. */
int dns_resolve_or_parse(const char *host, u32 *out_ip);

#endif
