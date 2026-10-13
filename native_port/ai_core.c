#include "ai_core.h"
#include <math.h>
#include <string.h>

/* Behaviours, all measured in zombie radii so they scale with the world.
 *
 * Melee zombies
 *  - Encircling: once per frame the zombies closing in (except a share of
 *    direct "rushers") are ranked by their bearing around the player and
 *    given evenly spaced slots, keeping their order so paths don't cross.
 *    Each steers for its slot on a ring that shrinks as it approaches, so
 *    the horde fills the gaps around the player instead of forming a
 *    single conga line that is easy to kite and mow down.
 *  - Hammer and anvil: while a boss is closing in, the slots are laid out
 *    on an arc on the far side of the player from the boss, so backing
 *    away from the boss means backing into the horde.
 *  - Interception: aim where a moving player will be on arrival.
 *  - Separation: steer away from close neighbours, and out of the way of
 *    bigger enemies (bosses) in particular.
 *  All of it fades out as a zombie closes in (FADE_RADII) and is off within
 *  CLOSE_RADII, where the original target is used so the final lunge and
 *  attack trigger are unchanged.
 *
 * Throwers (ranged)
 *  - Hold position inside their throw range (STANDOFF of the attack radius)
 *    instead of walking into the player's guns, spread evenly around the
 *    player in a formation that slowly rotates so they are never still,
 *    and back off when the player rushes them.
 *
 * Bosses
 *  - Charge straight at the player with full interception; they are never
 *    pushed aside by minions.
 *
 * The steering target is placed a full chase distance away along the
 * desired direction where full speed is wanted, because Zombie::move slows
 * down within one radius of its target and snaps onto it. */

#define CLOSE_RADII        5.0f   /* original behaviour inside this distance */
#define FADE_RADII        12.0f   /* full strength beyond CLOSE + FADE */
#define SEPARATION_RADII   3.0f   /* neighbour influence radius */
#define MAX_LEAD_SECONDS   1.2f
#define MAX_TRACKED        512
#define STALE_SECONDS      1.0
#define FRESH_SECONDS      0.1    /* neighbours updated this recently count */
#define ENGAGE_RADII      12.0f   /* melee zombies this close fan out to encircle */
#define RING_FRACTION      0.85f  /* slot ring radius as a share of distance */
#define PLAN_INTERVAL      0.008  /* re-plan slots once per frame */

#define ANVIL_ARC_DEGREES 240.0f  /* minion arc opposite an engaged boss */
#define BOSS_ENGAGE_RADII 25.0f   /* boss this close switches minions to the anvil */

#define STANDOFF           0.85f  /* throwers hold this share of their range */
#define RETREAT            0.3f   /* and back off inside this share */
#define RANGED_APPROACH    1.3f   /* beyond this share they just close in */
#define FORMATION_LEAD     0.25f  /* rad each thrower aims ahead round the ring */
#define MAX_SLOT_STEP      0.5f   /* rad a thrower moves round its ring per plan */

typedef struct {
    uintptr_t id;
    AiRole role;
    AiVec2 pos;
    float radius;
    double last_t;
    float speed;    /* measured, units per second */
    float slot;     /* assigned bearing around the player (radians) */
    int has_slot;
} Tracked;

static AiConfig g_cfg;
static int g_cfg_set = 0;

static Tracked g_tracked[MAX_TRACKED];
static int g_tracked_count = 0;

static AiVec2 g_player_last;
static double g_player_last_t = -1.0;
static AiVec2 g_player_vel;
static double g_last_plan_t = -1.0;

void ai_config_default(AiConfig *cfg) {
    /* Tuned in simulation (twin-stick aimed gun, kiting / running /
     * standing player) to keep or raise pressure versus classic AI */
    cfg->enabled = 1;
    cfg->flank_fraction = 0.75f;
    cfg->flank_degrees = 60.0f;
    cfg->lead = 0.8f;
    cfg->separation = 0.4f;
    cfg->ranged_tactics = 1;
    cfg->boss_tactics = 1;
}

void ai_set_config(const AiConfig *cfg) {
    g_cfg = *cfg;
    g_cfg_set = 1;
}

const AiConfig *ai_get_config(void) {
    if (!g_cfg_set) {
        ai_config_default(&g_cfg);
        g_cfg_set = 1;
    }
    return &g_cfg;
}

void ai_reset(void) {
    g_tracked_count = 0;
    g_player_last_t = -1.0;
    g_player_vel.x = g_player_vel.y = 0.0f;
    g_last_plan_t = -1.0;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float len2(AiVec2 v) {
    return sqrtf(v.x * v.x + v.y * v.y);
}

static AiVec2 normalized(AiVec2 v) {
    float l = len2(v);
    AiVec2 r = { 0.0f, 0.0f };
    if (l > 1e-6f) {
        r.x = v.x / l;
        r.y = v.y / l;
    }
    return r;
}

static uint32_t hash_id(uintptr_t id) {
    uint32_t x = (uint32_t)id;
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Rushers charge straight in; the rest take part in encircling */
static int is_flanker(uintptr_t id, float fraction) {
    float u = (float)(hash_id(id) & 0xFFFF) / 65535.0f;
    return u < fraction;
}

static float wrap_angle(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

int ai_lookup_role(uintptr_t enemy_id, AiRole *role) {
    for (int i = 0; i < g_tracked_count; i++) {
        if (g_tracked[i].id == enemy_id) {
            *role = g_tracked[i].role;
            return 1;
        }
    }
    return 0;
}

static int is_fresh(const Tracked *t, double now) {
    return now - t->last_t <= FRESH_SECONDS;
}

static void track_player(AiVec2 ppos, double now) {
    if (g_player_last_t < 0.0) {
        g_player_last = ppos;
        g_player_last_t = now;
        return;
    }
    double dt = now - g_player_last_t;
    if (dt < 0.03) return; /* many zombies update per frame; sample ~30 Hz */
    AiVec2 v = { (float)((ppos.x - g_player_last.x) / dt), (float)((ppos.y - g_player_last.y) / dt) };
    if (dt > 0.5) {
        v.x = v.y = 0.0f; /* paused or teleported */
    }
    g_player_vel.x += (v.x - g_player_vel.x) * 0.3f;
    g_player_vel.y += (v.y - g_player_vel.y) * 0.3f;
    g_player_last = ppos;
    g_player_last_t = now;
}

static Tracked *track_enemy(uintptr_t id, AiRole role, AiVec2 zpos, float radius, double now) {
    Tracked *found = NULL;
    /* Find, and compact away entries for enemies no longer following */
    int w = 0;
    for (int i = 0; i < g_tracked_count; i++) {
        if (now - g_tracked[i].last_t > STALE_SECONDS && g_tracked[i].id != id) continue;
        if (w != i) g_tracked[w] = g_tracked[i];
        if (g_tracked[w].id == id) found = &g_tracked[w];
        w++;
    }
    g_tracked_count = w;

    if (!found) {
        if (g_tracked_count >= MAX_TRACKED) return NULL;
        found = &g_tracked[g_tracked_count++];
        memset(found, 0, sizeof(*found));
        found->id = id;
        found->pos = zpos;
        found->last_t = now;
    } else {
        double dt = now - found->last_t;
        if (dt > 1e-4 && dt < 0.25) {
            AiVec2 d = { zpos.x - found->pos.x, zpos.y - found->pos.y };
            float inst = len2(d) / (float)dt;
            found->speed += (inst - found->speed) * 0.2f;
        }
        found->pos = zpos;
        found->last_t = now;
    }
    found->role = role;
    found->radius = radius;
    return found;
}

static void sort_by_key(int *order, const float *key, int n) {
    for (int a = 1; a < n; a++) {
        int k = order[a], b = a - 1;
        while (b >= 0 && key[order[b]] > key[k]) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = k;
    }
}

/* Melee: spread the encircling zombies around the player, keeping their
 * angular order. With a boss engaged they fill an arc on the far side of
 * the player from it; otherwise the whole circle, rotated as little as
 * possible from where they already are. */
static void plan_melee_slots(AiVec2 ppos, float r, double now) {
    static int order[MAX_TRACKED];
    static float bearing[MAX_TRACKED];
    static float key[MAX_TRACKED];
    const AiConfig *cfg = ai_get_config();
    float engage = ENGAGE_RADII * r;

    /* Nearest engaged boss, if any */
    int boss = -1;
    float boss_dist = BOSS_ENGAGE_RADII * r;
    if (cfg->boss_tactics) {
        for (int i = 0; i < g_tracked_count; i++) {
            const Tracked *t = &g_tracked[i];
            if (t->role != AI_ROLE_BOSS || !is_fresh(t, now)) continue;
            float d = len2((AiVec2){ t->pos.x - ppos.x, t->pos.y - ppos.y });
            if (d < boss_dist) {
                boss_dist = d;
                boss = i;
            }
        }
    }
    float anvil = 0.0f;
    if (boss >= 0) {
        anvil = atan2f(ppos.y - g_tracked[boss].pos.y, ppos.x - g_tracked[boss].pos.x);
    }

    int n = 0;
    for (int i = 0; i < g_tracked_count; i++) {
        Tracked *t = &g_tracked[i];
        if (t->role != AI_ROLE_MELEE) continue;
        t->has_slot = 0;
        if (!is_fresh(t, now) || !is_flanker(t->id, cfg->flank_fraction)) continue;
        AiVec2 dv = { t->pos.x - ppos.x, t->pos.y - ppos.y };
        if (len2(dv) > engage) continue;
        bearing[i] = atan2f(dv.y, dv.x);
        key[i] = boss >= 0 ? wrap_angle(bearing[i] - anvil) : bearing[i];
        order[n++] = i;
    }
    if (n < 2) return;
    sort_by_key(order, key, n);

    float slots_base, step;
    if (boss >= 0) {
        float arc = ANVIL_ARC_DEGREES * 3.14159265f / 180.0f;
        step = arc / (float)n;
        slots_base = anvil - arc * 0.5f + step * 0.5f;
    } else {
        /* Offset that best fits the evenly spaced pattern to current
         * bearings (circular mean of the residuals) */
        step = 6.2831853f / (float)n;
        float sx = 0.0f, sy = 0.0f;
        for (int k = 0; k < n; k++) {
            float res = bearing[order[k]] - step * (float)k;
            sx += cosf(res);
            sy += sinf(res);
        }
        slots_base = atan2f(sy, sx);
    }

    /* Limit how far a slot can pull a zombie round per plan, so a zombie on
     * the far side isn't sent on a long detour */
    float limit = cfg->flank_degrees * 3.14159265f / 180.0f;
    for (int k = 0; k < n; k++) {
        Tracked *t = &g_tracked[order[k]];
        float ideal = wrap_angle(slots_base + step * (float)k);
        float delta = clampf(wrap_angle(ideal - bearing[order[k]]), -limit, limit);
        t->slot = bearing[order[k]] + delta;
        t->has_slot = 1;
    }
}

/* Ranged: spread throwers evenly around the player in a formation that
 * rotates slowly, so they attack from different sides and keep moving. */
static void plan_ranged_slots(AiVec2 ppos, double now) {
    static int order[MAX_TRACKED];
    static float bearing[MAX_TRACKED];
    int n = 0;
    for (int i = 0; i < g_tracked_count; i++) {
        Tracked *t = &g_tracked[i];
        if (t->role != AI_ROLE_RANGED) continue;
        t->has_slot = 0;
        if (!is_fresh(t, now)) continue;
        bearing[i] = atan2f(t->pos.y - ppos.y, t->pos.x - ppos.x);
        order[n++] = i;
    }
    if (n == 0) return;
    sort_by_key(order, bearing, n);

    float step = 6.2831853f / (float)n;
    float sx = 0.0f, sy = 0.0f;
    for (int k = 0; k < n; k++) {
        float res = bearing[order[k]] - step * (float)k;
        sx += cosf(res);
        sy += sinf(res);
    }
    /* Aim each slot a little ahead so the formation keeps circling */
    float base = atan2f(sy, sx) + FORMATION_LEAD;
    for (int k = 0; k < n; k++) {
        Tracked *t = &g_tracked[order[k]];
        float ideal = wrap_angle(base + step * (float)k);
        t->slot = bearing[order[k]] + clampf(wrap_angle(ideal - bearing[order[k]]), -MAX_SLOT_STEP, MAX_SLOT_STEP);
        t->has_slot = 1;
    }
}

static AiVec2 intercept_point(const Tracked *self, AiVec2 ppos, float d, float lead, float w) {
    AiVec2 aim = ppos;
    if (lead > 0.0f && self->speed > 1e-3f) {
        float t = clampf(d / self->speed, 0.0f, MAX_LEAD_SECONDS) * lead * w;
        AiVec2 off = { g_player_vel.x * t, g_player_vel.y * t };
        float off_len = len2(off);
        if (off_len > 0.5f * d) { /* never lead further than half the gap */
            off.x *= 0.5f * d / off_len;
            off.y *= 0.5f * d / off_len;
        }
        aim.x += off.x;
        aim.y += off.y;
    }
    return aim;
}

/* Push away from close neighbours; bigger neighbours push harder, so
 * minions clear a path for bosses. */
static AiVec2 separation_push(const Tracked *self, AiVec2 zpos, float r, double now) {
    AiVec2 push = { 0.0f, 0.0f };
    for (int i = 0; i < g_tracked_count; i++) {
        const Tracked *o = &g_tracked[i];
        if (o == self || !is_fresh(o, now)) continue;
        float other_r = o->radius > r ? o->radius : r;
        float reach = SEPARATION_RADII * other_r;
        AiVec2 dv = { zpos.x - o->pos.x, zpos.y - o->pos.y };
        float dist = len2(dv);
        if (dist >= reach || dist < 1e-5f) continue;
        float k = (1.0f - dist / reach) / dist * (other_r / r);
        push.x += dv.x * k;
        push.y += dv.y * k;
    }
    return push;
}

static AiVec2 steer_melee(Tracked *self, AiVec2 zpos, float r, AiVec2 ppos, float d, double now) {
    const AiConfig *cfg = ai_get_config();
    AiVec2 to_player = { ppos.x - zpos.x, ppos.y - zpos.y };

    /* Fades every behaviour in with distance beyond the close range */
    float w = clampf((d - CLOSE_RADII * r) / (FADE_RADII * r), 0.0f, 1.0f);
    AiVec2 aim = intercept_point(self, ppos, d, cfg->lead, w);

    /* Encircling: steer for this zombie's slot on a shrinking ring */
    if (self->has_slot) {
        float ring = d * RING_FRACTION;
        AiVec2 slot_pt = { aim.x + cosf(self->slot) * ring, aim.y + sinf(self->slot) * ring };
        /* blend from direct chase (close) to full slot steering (far) */
        aim.x += (slot_pt.x - aim.x) * w;
        aim.y += (slot_pt.y - aim.y) * w;
    }
    AiVec2 dir = normalized((AiVec2){ aim.x - zpos.x, aim.y - zpos.y });

    if (cfg->separation > 0.0f) {
        AiVec2 push = separation_push(self, zpos, r, now);
        float sw = cfg->separation * w;
        dir.x += push.x * sw;
        dir.y += push.y * sw;
        /* Never let separation turn a zombie away from the player */
        AiVec2 fwd = normalized(to_player);
        if (dir.x * fwd.x + dir.y * fwd.y < 0.2f) {
            dir.x += fwd.x * 0.5f;
            dir.y += fwd.y * 0.5f;
        }
        dir = normalized(dir);
    }

    /* Full chase distance away so Zombie::move keeps full speed */
    return (AiVec2){ zpos.x + dir.x * d, zpos.y + dir.y * d };
}

static AiVec2 steer_ranged(Tracked *self, AiVec2 zpos, float r, float attack_radius, AiVec2 ppos, float d, double now) {
    const AiConfig *cfg = ai_get_config();
    float standoff = attack_radius * STANDOFF;
    AiVec2 away = normalized((AiVec2){ zpos.x - ppos.x, zpos.y - ppos.y });

    /* Out of range: close in like any zombie (with interception) */
    if (d > attack_radius * RANGED_APPROACH || !self->has_slot) {
        AiVec2 aim = intercept_point(self, ppos, d, cfg->lead, 1.0f);
        AiVec2 dir = normalized((AiVec2){ aim.x - zpos.x, aim.y - zpos.y });
        return (AiVec2){ zpos.x + dir.x * d, zpos.y + dir.y * d };
    }

    /* Player is rushing us: back off at full speed */
    if (d < attack_radius * RETREAT) {
        AiVec2 tangent = { -away.y, away.x };
        float side = (hash_id(self->id) & 1) ? 1.0f : -1.0f;
        AiVec2 dir = normalized((AiVec2){ away.x + tangent.x * side * 0.3f, away.y + tangent.y * side * 0.3f });
        float dist = standoff > 4.0f * r ? standoff : 4.0f * r;
        return (AiVec2){ zpos.x + dir.x * dist, zpos.y + dir.y * dist };
    }

    /* In range: walk round the standoff ring towards this thrower's slot.
     * The target sits on the ring, so it eases to a halt at its post. */
    float bearing = atan2f(away.y, away.x);
    float step = clampf(wrap_angle(self->slot - bearing), -MAX_SLOT_STEP, MAX_SLOT_STEP);
    float a = bearing + step;
    AiVec2 post = { ppos.x + cosf(a) * standoff, ppos.y + sinf(a) * standoff };
    AiVec2 to_post = { post.x - zpos.x, post.y - zpos.y };
    if (len2(to_post) < 0.05f * r) {
        /* Never land exactly on it: Zombie::move snaps onto its target */
        post.x += away.y * 0.1f * r;
        post.y -= away.x * 0.1f * r;
    }
    return post;
}

static AiVec2 steer_boss(Tracked *self, AiVec2 zpos, float r, AiVec2 ppos, float d) {
    /* Relentless: full interception, no detours, never pushed aside */
    float w = clampf((d - CLOSE_RADII * r) / (FADE_RADII * r), 0.0f, 1.0f);
    AiVec2 aim = intercept_point(self, ppos, d, 1.0f, w);
    AiVec2 dir = normalized((AiVec2){ aim.x - zpos.x, aim.y - zpos.y });
    return (AiVec2){ zpos.x + dir.x * d, zpos.y + dir.y * d };
}

AiVec2 ai_follow_target(uintptr_t enemy_id, AiRole role, AiVec2 zpos, float zradius,
                        float attack_radius, AiVec2 ppos, double now_s) {
    const AiConfig *cfg = ai_get_config();
    if (!cfg->enabled) return ppos;
    if (role == AI_ROLE_RANGED && (!cfg->ranged_tactics || attack_radius <= 0.0f)) role = AI_ROLE_MELEE;
    if (role == AI_ROLE_BOSS && !cfg->boss_tactics) role = AI_ROLE_MELEE;

    float r = zradius > 1e-3f ? zradius : 1e-3f;
    track_player(ppos, now_s);
    Tracked *self = track_enemy(enemy_id, role, zpos, r, now_s);
    if (!self) return ppos;

    if (g_last_plan_t < 0.0 || now_s - g_last_plan_t > PLAN_INTERVAL) {
        plan_melee_slots(ppos, r, now_s);
        plan_ranged_slots(ppos, now_s);
        g_last_plan_t = now_s;
    }

    float d = len2((AiVec2){ ppos.x - zpos.x, ppos.y - zpos.y });

    if (role == AI_ROLE_RANGED) return steer_ranged(self, zpos, r, attack_radius, ppos, d, now_s);
    if (d < CLOSE_RADII * r) return ppos;
    if (role == AI_ROLE_BOSS) return steer_boss(self, zpos, r, ppos, d);
    return steer_melee(self, zpos, r, ppos, d, now_s);
}
