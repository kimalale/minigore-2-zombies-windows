#ifndef NET_PROTO_H
#define NET_PROTO_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Socket helpers; include this before <windows.h>. Plain protocol types
 * live in net_types.h so other code can use them without Winsock. */
#include "net_types.h"

int net_startup(void);
/* Sends one message made of up to two payload parts. 0 on success. */
int net_send_msg(SOCKET s, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen);
/* Receives one message into a malloc'd buffer (caller frees). 0 on success. */
int net_recv_msg(SOCKET s, uint8_t *type, uint8_t **payload, uint32_t *len);
void net_set_nodelay(SOCKET s);

#ifdef __cplusplus
}
#endif

#endif /* NET_PROTO_H */
