#ifndef LOCKSTEP_H
#define LOCKSTEP_H

#include <stddef.h>
#include <stdint.h>
#include "net_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Own-world co-op ("lockstep"): each player runs the whole game on their
 * own PC and only inputs travel over the network. Both games start from
 * the same saves and settings, run on the determinism test's virtual
 * clock, and apply the same inputs on the same frame - so both worlds
 * stay identical without streaming any video:
 *
 *   player 1 (host)  - the game's own player; drives the menus too
 *   player 2 (guest) - a companion character in both worlds (coop.c)
 *
 * Each side's input is applied `delay` frames after it is sampled, which
 * hides the network round trip. A fingerprint of both worlds is compared
 * every second to detect any divergence. */

typedef enum { LS_OFF = 0, LS_HOST, LS_GUEST } LsRole;

typedef struct {
    LsRole role;
    char host[256];           /* guest: who to join */
    int port;
    char code[NET_CODE_LEN];  /* optional join code */
    int delay;                /* host: input delay in frames */
} LockstepConfig;

void lockstep_config_default(LockstepConfig *cfg);

/* Before the game starts (and before its window is created):
 *   host  - waits for the guest, then sends `setup` and its saves
 *   guest - connects, receives the host's `setup` and saves
 * Returns 0 when both are ready to start. */
int lockstep_connect(const LockstepConfig *cfg, MsgLsSetup *setup, const char *build, uint32_t game_hash);

/* Guest: where the host's saves were written (NULL on the host) */
const char *lockstep_data_path(void);

int lockstep_active(void);
int lockstep_is_guest(void);
int lockstep_peer_present(void);
void lockstep_shutdown(void);

/* Game thread, from det_frame_end: `next` is the frame about to start.
 * `fp` is this frame's fingerprint every 60 frames, else NULL. */
void lockstep_frame_end(uint32_t next, const MsgLsHash *fp);

/* Game thread, during a frame: this frame's inputs */
int lockstep_p1_sticks(float sticks[4]);     /* 1 if live */
int lockstep_p2_sticks(float sticks[4]);     /* 1 while player 2 is connected */
int lockstep_event_pending(void);
int lockstep_take_event(void *event, size_t size);

/* For the overlay */
int lockstep_out_of_sync(void);
int lockstep_waiting(void);

#ifdef __cplusplus
}
#endif

#endif /* LOCKSTEP_H */
