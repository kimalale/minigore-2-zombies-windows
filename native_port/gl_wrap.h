#ifndef GL_WRAP_H
#define GL_WRAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_wrap_init(void);

/* Requested anisotropic filtering level (<= 1 disables). Call before the
 * GL context is created; applied to mipmapped textures. */
void gl_wrap_set_anisotropy(int level);

/* Framebuffer the game gets when it binds framebuffer 0 (0 = the window). */
void gl_wrap_set_default_framebuffer(unsigned int fbo);

#ifdef __cplusplus
}
#endif

#endif /* GL_WRAP_H */
