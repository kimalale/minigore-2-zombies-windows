#ifndef GUEST_H
#define GUEST_H

#include "net_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char host[256];
    int port;
    char code[NET_CODE_LEN];
} GuestConfig;

/* Runs the co-op guest (player 2) client until its window is closed.
 * Does not load the game: it shows the host's stream and sends input. */
int guest_main(const GuestConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* GUEST_H */
