#ifndef UI_NAV_H
#define UI_NAV_H

#include "elf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Menu navigation for controllers. The game's menus are an immediate-mode
 * UI (xt::Imgui) with no focus system; every button calls
 * xt::Imgui::buttonLogic(id, x, y, w, h) each frame. Hooking it gives the
 * on-screen buttons (in window pixels, y down), so the D-pad can jump the
 * cursor between them. */
void ui_nav_install(elf_module_t *mod);

/* From (cx, cy), the centre of the nearest button in direction (dx, dy)
 * (one of the four D-pad directions, screen y down). With dx = dy = 0,
 * the nearest button in any direction. Returns 0 if there is none. */
int ui_nav_find(float cx, float cy, int dx, int dy, float *tx, float *ty);

#ifdef __cplusplus
}
#endif

#endif /* UI_NAV_H */
