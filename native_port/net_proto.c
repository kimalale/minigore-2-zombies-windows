#include "net_proto.h"
#include <stdlib.h>
#include <string.h>

int net_startup(void) {
    static int started = 0;
    if (started) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    started = 1;
    return 0;
}

static int send_all(SOCKET s, const void *data, uint32_t len) {
    const char *p = (const char *)data;
    while (len > 0) {
        int n = send(s, p, len > 0x10000 ? 0x10000 : (int)len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (uint32_t)n;
    }
    return 0;
}

static int recv_all(SOCKET s, void *data, uint32_t len) {
    char *p = (char *)data;
    while (len > 0) {
        int n = recv(s, p, len > 0x10000 ? 0x10000 : (int)len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (uint32_t)n;
    }
    return 0;
}

int net_send_msg(SOCKET s, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    uint8_t header[5];
    uint32_t total = alen + blen;
    header[0] = type;
    memcpy(header + 1, &total, 4);
    if (send_all(s, header, sizeof(header)) != 0) return -1;
    if (alen && send_all(s, a, alen) != 0) return -1;
    if (blen && send_all(s, b, blen) != 0) return -1;
    return 0;
}

int net_recv_msg(SOCKET s, uint8_t *type, uint8_t **payload, uint32_t *len) {
    uint8_t header[5];
    if (recv_all(s, header, sizeof(header)) != 0) return -1;
    uint32_t n;
    memcpy(&n, header + 1, 4);
    if (n > NET_MAX_MESSAGE) return -1;
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
    if (!buf) return -1;
    if (n && recv_all(s, buf, n) != 0) {
        free(buf);
        return -1;
    }
    *type = header[0];
    *payload = buf;
    *len = n;
    return 0;
}

void net_set_nodelay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
}
