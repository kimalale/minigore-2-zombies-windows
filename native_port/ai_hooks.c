#include "ai_hooks.h"
#include "ai_core.h"
#include "coop.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ZombieStateFollow::update(float) begins every frame with
 *
 *   +0x39  mov    ecx, [ecx+0x1f4fa0]   ; ecx = character being chased
 *   +0x3f  movsd  xmm0, [ecx+0x4]       ; its position (Vector2)
 *   +0x44  movsd  [eax+0x48], xmm0      ; -> zombie's steering target
 *
 * and then Zombie::face() / Zombie::move() steer towards Zombie+0x48.
 * The two movsd instructions (10 bytes) are replaced with a call to
 * follow_stub + 5 NOPs. At that point eax = Zombie*, ecx = chased
 * character, and [ebp+0xc] is the frame's dt. The stub preserves every
 * general purpose and XMM register (xmm1 holds dt for the next call) and
 * lets ai_core choose the target.
 *
 * Attack triggering is a separate distance check against each player
 * character later in the same function, so it is unaffected. */

#define FOLLOW_UPDATE_SYMBOL "_ZN17ZombieStateFollow6updateEf"
#define PATCH_OFFSET 0x3f

static const uint8_t k_expected[10] = {
    0xf2, 0x0f, 0x10, 0x41, 0x04,   /* movsd xmm0, qword ptr [ecx+0x4]  */
    0xf2, 0x0f, 0x11, 0x40, 0x48,   /* movsd qword ptr [eax+0x48], xmm0 */
};

/* Game structure offsets (see ZombieStateFollow::update / Zombie::move) */
#define ENTITY_POS       0x04   /* Vector2 position */
#define ENTITY_TYPE      0x2c   /* uint64 entity type flags */
#define ZOMBIE_RADIUS    0x24   /* collision radius */
#define ZOMBIE_TARGET    0x48   /* Vector2 steering target */
#define ZOMBIE_BEHAVIOR  0x13c  /* Behavior* (class decides how it fights) */
#define VTBL_GET_ATTACK_RADIUS 0x34 /* Behavior::getAttackRadius() const */

/* Thrower variants with a fixed long throw range (BehaviorThrower::
 * getAttackRadius returns 250 for these; others melee or roll randomly) */
#define TYPE_THROWER_A   0x400ull
#define TYPE_THROWER_B   0x800ull

typedef float (__attribute__((cdecl)) *GetAttackRadiusFn)(void *self);

static LARGE_INTEGER g_qpc_freq;
static uintptr_t g_vtbl_thrower = 0; /* address stored in objects: vtable + 8 */
static uintptr_t g_vtbl_boss = 0;

void follow_stub(void) __asm__("ai_follow_stub");
void ai_follow_entry(uint8_t *zombie, uint8_t *chased) __asm__("ai_follow_entry");

__attribute__((used, force_align_arg_pointer))
void ai_follow_entry(uint8_t *zombie, uint8_t *chased) {
    const float *zp = (const float *)(zombie + ENTITY_POS);
    /* Co-op: chase whichever player is closer (the game only ever chases
     * player 1; attacks already hit any player in reach) */
    uint8_t *p2 = (uint8_t *)coop_p2_live();
    if (p2) {
        const float *a = (const float *)(chased + ENTITY_POS);
        const float *b = (const float *)(p2 + ENTITY_POS);
        float da = (a[0] - zp[0]) * (a[0] - zp[0]) + (a[1] - zp[1]) * (a[1] - zp[1]);
        float db = (b[0] - zp[0]) * (b[0] - zp[0]) + (b[1] - zp[1]) * (b[1] - zp[1]);
        if (db < da) chased = p2;
    }
    const float *pp = (const float *)(chased + ENTITY_POS);
    float *target = (float *)(zombie + ZOMBIE_TARGET);
    float radius = *(const float *)(zombie + ZOMBIE_RADIUS);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = (double)now.QuadPart / (double)g_qpc_freq.QuadPart;

    /* Classify by Behavior class */
    AiRole role = AI_ROLE_MELEE;
    float attack_radius = 0.0f;
    void *behavior = *(void **)(zombie + ZOMBIE_BEHAVIOR);
    if (behavior) {
        uintptr_t vtbl = *(uintptr_t *)behavior;
        if (vtbl == g_vtbl_boss) {
            role = AI_ROLE_BOSS;
        } else if (vtbl == g_vtbl_thrower) {
            uint64_t type = *(const uint64_t *)(zombie + ENTITY_TYPE);
            if (type == TYPE_THROWER_A || type == TYPE_THROWER_B) {
                GetAttackRadiusFn fn = *(GetAttackRadiusFn *)(vtbl + VTBL_GET_ATTACK_RADIUS);
                attack_radius = fn(behavior);
                if (attack_radius > radius * 3.0f) role = AI_ROLE_RANGED;
            }
        }
    }

    AiVec2 z = { zp[0], zp[1] };
    AiVec2 p = { pp[0], pp[1] };
    AiVec2 out = ai_follow_target((uintptr_t)zombie, role, z, radius, attack_radius, (uintptr_t)chased, p, t);
    if (out.x != out.x || out.y != out.y) {
        out = p; /* never hand the game a NaN */
    }
    target[0] = out.x;
    target[1] = out.y;
}

/* Called in place of the two movsd instructions: eax = Zombie*,
 * ecx = chased character. Saves all GPRs (pushal) and xmm0-7. */
__asm__(
    ".text\n"
    ".globl ai_follow_stub\n"
    "ai_follow_stub:\n"
    "    pushal\n"
    "    subl   $128, %esp\n"
    "    movdqu %xmm0,   0(%esp)\n"
    "    movdqu %xmm1,  16(%esp)\n"
    "    movdqu %xmm2,  32(%esp)\n"
    "    movdqu %xmm3,  48(%esp)\n"
    "    movdqu %xmm4,  64(%esp)\n"
    "    movdqu %xmm5,  80(%esp)\n"
    "    movdqu %xmm6,  96(%esp)\n"
    "    movdqu %xmm7, 112(%esp)\n"
    "    pushl  %ecx\n"
    "    pushl  %eax\n"
    "    call   ai_follow_entry\n"
    "    addl   $8, %esp\n"
    "    movdqu   0(%esp), %xmm0\n"
    "    movdqu  16(%esp), %xmm1\n"
    "    movdqu  32(%esp), %xmm2\n"
    "    movdqu  48(%esp), %xmm3\n"
    "    movdqu  64(%esp), %xmm4\n"
    "    movdqu  80(%esp), %xmm5\n"
    "    movdqu  96(%esp), %xmm6\n"
    "    movdqu 112(%esp), %xmm7\n"
    "    addl   $128, %esp\n"
    "    popal\n"
    "    ret\n"
);

int ai_hooks_install(elf_module_t *mod) {
    const AiConfig *cfg = ai_get_config();
    if (!cfg->enabled) {
        printf("[AI] Classic zombie AI (smart AI disabled)\n");
        return 0;
    }

    uintptr_t fn = elf_sym(mod, FOLLOW_UPDATE_SYMBOL);
    if (!fn) {
        printf("[AI] %s not found; keeping classic AI\n", FOLLOW_UPDATE_SYMBOL);
        return 0;
    }
    uint8_t *site = (uint8_t *)(fn + PATCH_OFFSET);
    if (memcmp(site, k_expected, sizeof(k_expected)) != 0) {
        printf("[AI] Unexpected code at ZombieStateFollow::update+0x%x; keeping classic AI\n", PATCH_OFFSET);
        return 0;
    }

    QueryPerformanceFrequency(&g_qpc_freq);
    uintptr_t vt;
    if ((vt = elf_sym(mod, "_ZTV15BehaviorThrower")) != 0) g_vtbl_thrower = vt + 8;
    if ((vt = elf_sym(mod, "_ZTV12BehaviorBoss")) != 0) g_vtbl_boss = vt + 8;

    DWORD old_protect;
    if (!VirtualProtect(site, sizeof(k_expected), PAGE_EXECUTE_READWRITE, &old_protect)) {
        printf("[AI] VirtualProtect failed (%lu); keeping classic AI\n", GetLastError());
        return 0;
    }
    int32_t rel = (int32_t)((uintptr_t)follow_stub - ((uintptr_t)site + 5));
    site[0] = 0xE8; /* call rel32 */
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, 5); /* nop */
    VirtualProtect(site, sizeof(k_expected), old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(k_expected));

    printf("[AI] Smart zombie AI enabled: %.0f%% encircle (up to %.0f deg), lead %.1f, separation %.1f, "
           "thrower tactics %s, boss tactics %s\n",
           cfg->flank_fraction * 100.0f, cfg->flank_degrees, cfg->lead, cfg->separation,
           (cfg->ranged_tactics && g_vtbl_thrower) ? "on" : "off",
           (cfg->boss_tactics && g_vtbl_boss) ? "on" : "off");
    fflush(stdout);
    return 1;
}
