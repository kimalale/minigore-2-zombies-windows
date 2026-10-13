#ifndef DETERMINISM_H
#define DETERMINISM_H

#include <stddef.h>
#include <stdint.h>
#include "elf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Determinism test: does the game produce exactly the same state from the
 * same inputs? (the prerequisite for lockstep multiplayer, where each
 * player runs their own copy of the game and only inputs are exchanged)
 *
 *   --record <file>  play normally; every input the game consumes and a
 *                    fingerprint of the game state are saved per frame
 *   --replay <file>  replays the inputs without a human and compares the
 *                    fingerprints frame by frame, reporting any desync
 *
 * In both modes every other source of variation is pinned: a virtual clock
 * advancing exactly 1/60 s per frame, a fixed date, the same save data
 * (a snapshot taken when recording), and focus changes ignored. */

typedef enum { DET_OFF = 0, DET_RECORD, DET_REPLAY, DET_LOCKSTEP } DetMode;

void det_configure(DetMode mode, const char *path);
int det_active(void);

/* Before the game starts: copies the save snapshot into a scratch folder
 * and returns the data path the game should use (NULL = normal saves/). */
const char *det_prepare_saves(void);

/* After relocation: resolves game functions used for fingerprints. */
void det_install(elf_module_t *mod);

/* Virtual clock (only meaningful while active) */
uint64_t det_now_ns(void);
void det_advance_ns(uint64_t ns);       /* game sleeps */
void det_idle(void);                    /* a game-loop pass that drew nothing */
double det_sim_seconds(void);           /* for the AI */
uint32_t det_frame(void);               /* frames presented so far */

/* Both random generators, for drawing that must not consume them */
typedef struct { long long rand48; uint32_t random; } DetRng;
void det_rng_save(DetRng *r);
void det_rng_restore(const DetRng *r);
int64_t det_wall_time(void);            /* fixed date */

/* Game thread hooks */
void det_frame_end(void);                               /* eglSwapBuffers */
void det_sticks(float sticks[4]);                       /* after updateControls */
int det_replay_event(void *event, size_t size);         /* 1 = delivered a recorded event */
int det_replaying(void);
int det_replay_pending(void);                           /* events due this frame */
void det_record_event(const void *event, size_t size);  /* an event the game consumed */

/* Window size (recorded; checked on replay) */
void det_set_screen(int w, int h);

/* Shutdown summary */
void det_finish(void);

#ifdef __cplusplus
}
#endif

#endif /* DETERMINISM_H */
