#ifndef AI_CORE_H
#define AI_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zombie steering ("smart AI"), independent of the game binary so it can be
 * tested on its own. ai_hooks.c feeds it positions read from the game. */

typedef struct {
    float x, y;
} AiVec2;

typedef struct {
    int   enabled;          /* 0 = original straight-line chase */
    float flank_fraction;   /* share of zombies that circle round to flank, 0..1 */
    float flank_degrees;    /* max angle a flanker veers off the direct line */
    float lead;             /* 0..1, how far ahead of a moving player to aim */
    float separation;       /* strength of spreading out from neighbours */
} AiConfig;

void ai_config_default(AiConfig *cfg);
void ai_set_config(const AiConfig *cfg);
const AiConfig *ai_get_config(void);

/* Returns where a following zombie should steer this frame.
 *   zombie_id  stable identity (the Zombie pointer)
 *   zpos       zombie position; zradius its collision radius (sets the scale)
 *   ppos       position of the player it is chasing
 *   now_s      monotonic time in seconds
 * Within a few radii of the player the original target (ppos) is returned
 * unchanged, so the final approach and attack logic behave as before. */
AiVec2 ai_follow_target(uintptr_t zombie_id, AiVec2 zpos, float zradius, AiVec2 ppos, double now_s);

/* Forget all tracked state (level change / tests). */
void ai_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_CORE_H */
