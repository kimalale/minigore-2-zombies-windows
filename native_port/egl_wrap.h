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

#ifdef __cplusplus
}
#endif

#endif /* EGL_WRAP_H */
