#ifndef EGL_WRAP_H
#define EGL_WRAP_H

#include <stdint.h>
#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void egl_wrap_init(SDL_Window *window);
void egl_wrap_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* EGL_WRAP_H */
