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

#ifdef __cplusplus
}
#endif

#endif /* GL_WRAP_H */
