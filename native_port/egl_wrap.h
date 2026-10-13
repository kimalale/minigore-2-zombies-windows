#ifndef EGL_WRAP_H
#define EGL_WRAP_H

#include <stdint.h>
#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void egl_wrap_init(SDL_Window *window);
void egl_wrap_shutdown(void);

/* Window was created with a multisampled pixel format */
void egl_wrap_set_msaa(int enabled);

/* Controller menu cursor, drawn over the game (window pixels). */
void egl_overlay_set_cursor(float x, float y, int visible, int pressed);

/* Own-world co-op: both players' health and where the partner is. Set by
 * the game thread each frame of a level; visible = 0 hides it. */
typedef struct {
    int visible;
    int local;                /* 0: this PC plays player 1, 1: player 2 */
    struct {
        int present, alive;
        float health;         /* 0..1 */
        int on_screen_valid;
        float sx, sy;         /* window pixels */
    } p[2];
    int out_of_sync;
    int waiting;
} EglCoopOverlay;
void egl_overlay_set_coop(const EglCoopOverlay *info);

#ifdef __cplusplus
}
#endif

#endif /* EGL_WRAP_H */
