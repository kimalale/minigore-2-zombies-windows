#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <windows.h>
#include "elf_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the hang detector. If no frame is presented for several seconds
 * while the window has focus, every registered thread is sampled and a
 * report (registers + symbolized stack scan) is written to hang_report.txt. */
void watchdog_start(const elf_module_t *mod);

/* Tracks a game thread for hang reports. The handle is duplicated, so the
 * caller keeps ownership of its own. Safe to call before watchdog_start. */
void watchdog_register_thread(HANDLE thread, const char *name);

/* Called once per presented frame. */
void watchdog_frame(void);

/* The game legitimately stops presenting while unfocused / paused. */
void watchdog_set_focused(int focused);

#ifdef __cplusplus
}
#endif

#endif /* WATCHDOG_H */
