#ifndef NET_HOST_H
#define NET_HOST_H

#include "net_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int port;
    char code[NET_CODE_LEN];  /* empty = anyone may join */
    int width;                /* streamed video width in pixels */
    int fps;
    int quality;              /* JPEG quality 1-100 */
} NetHostConfig;

void net_host_config_default(NetHostConfig *cfg);

/* Starts listening for a guest in the background. 0 on success. */
int net_host_start(const NetHostConfig *cfg);
int net_host_active(void);
int net_host_guest_connected(void);

/* Latest guest input if received within max_age_ms. */
int net_host_get_input(MsgInput *out, unsigned max_age_ms);

/* Game thread, inside eglSwapBuffers with the GL context current. */
void net_host_capture_frame(int width, int height);
/* Game thread: player 2's on-screen position (0..1) for guest mouse aim. */
void net_host_set_p2(float x, float y, int alive, int in_level);
/* Game thread: player 2's health (0..1) and whether frames show their own view. */
void net_host_set_p2_status(float health, int own_view);
/* Game thread: framebuffer to stream from (0 = the window). */
void net_host_set_capture_source(unsigned int fbo);
/* Audio thread: the final 44.1 kHz stereo s16 mix. */
void net_host_audio(const void *pcm, int bytes);

#ifdef __cplusplus
}
#endif

#endif /* NET_HOST_H */
