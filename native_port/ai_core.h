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

/* How an enemy fights, decided by ai_hooks.c from its Behavior class */
typedef enum {
    AI_ROLE_MELEE = 0,  /* regular zombies: encircle the player */
    AI_ROLE_RANGED,     /* bomb / poison throwers: hold range and strafe */
    AI_ROLE_BOSS        /* bosses: charge straight in; minions form the anvil */
} AiRole;

typedef struct {
    int   enabled;          /* 0 = original straight-line chase */
    float flank_fraction;   /* share of melee zombies that encircle, 0..1 */
    float flank_degrees;    /* max angle a slot can pull a zombie round per frame */
    float lead;             /* 0..1, how far ahead of a moving player to aim */
    float separation;       /* strength of spreading out from neighbours */
    int   ranged_tactics;   /* throwers keep their distance and strafe */
    int   boss_tactics;     /* bosses charge, minions close in from the far side */
} AiConfig;

void ai_config_default(AiConfig *cfg);
void ai_set_config(const AiConfig *cfg);
const AiConfig *ai_get_config(void);

/* Returns where a following enemy should steer this frame.
 *   enemy_id       stable identity (the Zombie pointer)
 *   role           how it fights
 *   zpos, zradius  position and collision radius (sets the scale)
 *   attack_radius  distance from the player at which it can attack
 *   ppos           position of the player it is chasing
 *   now_s          monotonic time in seconds
 * Melee and boss enemies use the original target (ppos) within a few radii
 * of the player, so the final lunge and attack logic behave as before. */
AiVec2 ai_follow_target(uintptr_t enemy_id, AiRole role, AiVec2 zpos, float zradius,
                        float attack_radius, AiVec2 ppos, double now_s);

/* Role of an enemy that has followed the player recently (for aim
 * assist). Returns 0 if it isn't tracked. */
int ai_lookup_role(uintptr_t enemy_id, AiRole *role);

/* Forget all tracked state (level change / tests). */
void ai_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_CORE_H */
