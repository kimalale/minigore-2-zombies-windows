#include "java_stubs.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Most xt::java helpers check Env::jni before using it, but six don't,
 * so on Windows (Env::jni is NULL - the dummy JavaVM refuses to attach)
 * they crash with a NULL read inside _JNIEnv::Call*Method. Ad rewards are
 * polled from the pattern / ad updates during play, which crashed the game
 * partway through a level. Each helper's entry is replaced with a jump to
 * a stub below, which covers direct calls as well as calls via the GOT. */

typedef struct {
    const char *symbol;
    void *stub;
} JavaStub;

static int __attribute__((cdecl)) stub_no(void) {
    return 0; /* no rewarded video available / pending / shown */
}

static void __attribute__((cdecl)) stub_popup(const char *text) {
    printf("[Game] Message: %s\n", text ? text : "");
}

static void __attribute__((cdecl)) stub_browser(const char *url) {
    printf("[Game] Wanted to open a web page (not opened): %s\n", url ? url : "");
}

static const JavaStub k_stubs[] = {
    { "_ZN2xt4java26isRewardedVideoAdAvailableEv", (void *)stub_no },
    { "_ZN2xt4java25fetchVideoAdRewardPendingEv",  (void *)stub_no },
    { "_ZN2xt4java22displayRewardedVideoAdEv",     (void *)stub_no },
    { "_ZN2xt4java12displayPopupEPKc",             (void *)stub_popup },
    { "_ZN2xt4java13displayDialogEPKc",            (void *)stub_popup },
    { "_ZN2xt4java13launchBrowserEPKc",            (void *)stub_browser },
};

void java_stubs_install(elf_module_t *mod) {
    int patched = 0;
    for (size_t i = 0; i < sizeof(k_stubs) / sizeof(k_stubs[0]); i++) {
        uint8_t *fn = (uint8_t *)elf_sym(mod, k_stubs[i].symbol);
        /* Expect the usual "push ebp; mov ebp, esp" prologue */
        if (!fn || fn[0] != 0x55 || fn[1] != 0x89 || fn[2] != 0xE5) {
            printf("[JNI] Could not stub %s\n", k_stubs[i].symbol);
            continue;
        }
        DWORD old;
        if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
        int32_t rel = (int32_t)((uintptr_t)k_stubs[i].stub - ((uintptr_t)fn + 5));
        fn[0] = 0xE9; /* jmp rel32 */
        memcpy(fn + 1, &rel, 4);
        VirtualProtect(fn, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 5);
        patched++;
    }
    printf("[JNI] Stubbed %d Java helpers that would crash without Java\n", patched);
}
