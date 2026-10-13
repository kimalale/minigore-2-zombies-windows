#include "ai_core.h"
#include <math.h>
#include <string.h>

/* Behaviours, all measured in zombie radii so they scale with the world:
 *
 *  - Encircling: once per frame the zombies closing in (except a share of
 *    direct "rushers") are ranked by their bearing around the player and
 *    given evenly spaced slots, keeping their order so paths don't cross.
 *    Each steers for its slot on a ring that shrinks as it approaches, so
 *    the horde fills the gaps around the player instead of forming a
 *    single conga line that is easy to kite and mow down.
 *  - Interception: zombies aim where a moving player will be by the time
 *    they arrive (using each zombie's measured speed), rather than where
 *    the player was.
 *  - Separation: zombies steer away from close neighbours so they stay
 *    spread out rather than stacking behind each other.
 *
 * All of it fades out as a zombie closes in (FADE_RADII) and is off
 * entirely within CLOSE_RADII, where the original target is used so the
 * final lunge and attack trigger are unchanged. The steering target is
 * always placed a full chase distance away along the desired direction,
 * because Zombie::move slows down near its target and snaps onto it. */

#define CLOSE_RADII       5.0f   /* original behaviour inside this distance */
#define FADE_RADII       12.0f   /* full strength beyond CLOSE + FADE */
#define SEPARATION_RADII  3.0f   /* neighbour influence radius */
#define MAX_LEAD_SECONDS  1.2f
#define MAX_TRACKED       512
#define STALE_SECONDS     1.0
#define FRESH_SECONDS     0.1    /* neighbours updated this recently count */
#define ENGAGE_RADII     30.0f   /* zombies this close take part in encircling */
#define RING_FRACTION     0.7f   /* slot ring radius as a share of distance */
#define PLAN_INTERVAL     0.008  /* re-plan slots once per frame */

typedef struct {
    uintptr_t id;
    AiVec2 pos;
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
    /* Tuned in simulation against kiting, running and standing players */
    cfg->enabled = 1;
    cfg->flank_fraction = 0.75f;
    cfg->flank_degrees = 60.0f;
    cfg->lead = 0.8f;
    cfg->separation = 0.4f;
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

/* Spread the encircling zombies evenly around the player, keeping their
 * angular order and rotating the pattern as little as possible. */
static void plan_slots(AiVec2 ppos, float r, double now) {
    static int order[MAX_TRACKED];
    static float bearing[MAX_TRACKED];
    const AiConfig *cfg = ai_get_config();
    float engage = ENGAGE_RADII * r;
    int n = 0;

    for (int i = 0; i < g_tracked_count; i++) {
        Tracked *t = &g_tracked[i];
        t->has_slot = 0;
        if (now - t->last_t > FRESH_SECONDS || !is_flanker(t->id, cfg->flank_fraction)) continue;
        AiVec2 dv = { t->pos.x - ppos.x, t->pos.y - ppos.y };
        if (len2(dv) > engage) continue;
        bearing[i] = atan2f(dv.y, dv.x);
        order[n++] = i;
    }
    if (n < 2) return;

    /* insertion sort by bearing (n is small) */
    for (int a = 1; a < n; a++) {
        int key = order[a], b = a - 1;
        while (b >= 0 && bearing[order[b]] > bearing[key]) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = key;
    }

    /* Offset that best fits the evenly spaced pattern to current bearings
     * (circular mean of the residuals) */
    float step = 6.2831853f / (float)n;
    float sx = 0.0f, sy = 0.0f;
    for (int k = 0; k < n; k++) {
        float res = bearing[order[k]] - step * (float)k;
        sx += cosf(res);
        sy += sinf(res);
    }
    float base = atan2f(sy, sx);

    for (int k = 0; k < n; k++) {
        Tracked *t = &g_tracked[order[k]];
        float ideal = wrap_angle(base + step * (float)k);
        /* Limit how far a slot can pull a zombie round per plan, so a
         * zombie on the far side isn't sent on a long detour */
        float limit = cfg->flank_degrees * 3.14159265f / 180.0f;
        float delta = clampf(wrap_angle(ideal - bearing[order[k]]), -limit, limit);
        t->slot = bearing[order[k]] + delta;
        t->has_slot = 1;
    }
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

static Tracked *track_zombie(uintptr_t id, AiVec2 zpos, double now) {
    Tracked *found = NULL;
    /* Find, and compact away entries for zombies no longer following */
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
        found->id = id;
        found->pos = zpos;
        found->last_t = now;
        found->speed = 0.0f;
        return found;
    }

    double dt = now - found->last_t;
    if (dt > 1e-4 && dt < 0.25) {
        AiVec2 d = { zpos.x - found->pos.x, zpos.y - found->pos.y };
        float inst = len2(d) / (float)dt;
        found->speed += (inst - found->speed) * 0.2f;
    }
    found->pos = zpos;
    found->last_t = now;
    return found;
}

AiVec2 ai_follow_target(uintptr_t zombie_id, AiVec2 zpos, float zradius, AiVec2 ppos, double now_s) {
    const AiConfig *cfg = ai_get_config();
    if (!cfg->enabled) return ppos;

    float r = zradius > 1e-3f ? zradius : 1e-3f;
    track_player(ppos, now_s);
    Tracked *self = track_zombie(zombie_id, zpos, now_s);

    AiVec2 to_player = { ppos.x - zpos.x, ppos.y - zpos.y };
    float d = len2(to_player);
    float close = CLOSE_RADII * r;
    if (d < close || !self) return ppos;

    /* Fades every behaviour in with distance beyond the close range */
    float w = clampf((d - close) / (FADE_RADII * r), 0.0f, 1.0f);

    /* Interception: aim where the player will be on arrival */
    AiVec2 aim = ppos;
    if (cfg->lead > 0.0f && self->speed > 1e-3f) {
        float t = clampf(d / self->speed, 0.0f, MAX_LEAD_SECONDS) * cfg->lead * w;
        AiVec2 off = { g_player_vel.x * t, g_player_vel.y * t };
        float off_len = len2(off);
        if (off_len > 0.5f * d) { /* never lead further than half the gap */
            off.x *= 0.5f * d / off_len;
            off.y *= 0.5f * d / off_len;
        }
        aim.x += off.x;
        aim.y += off.y;
    }
    /* Encircling: steer for this zombie's slot on a shrinking ring */
    if (g_last_plan_t < 0.0 || now_s - g_last_plan_t > PLAN_INTERVAL) {
        plan_slots(ppos, r, now_s);
        g_last_plan_t = now_s;
    }
    if (self->has_slot) {
        float ring = d * RING_FRACTION;
        float slot = self->slot;
        AiVec2 slot_pt = { aim.x + cosf(slot) * ring, aim.y + sinf(slot) * ring };
        /* blend from direct chase (close) to full slot steering (far) */
        aim.x += (slot_pt.x - aim.x) * w;
        aim.y += (slot_pt.y - aim.y) * w;
    }
    AiVec2 dir = normalized((AiVec2){ aim.x - zpos.x, aim.y - zpos.y });

    /* Separation from neighbours that are following too */
    if (cfg->separation > 0.0f) {
        float reach = SEPARATION_RADII * r;
        AiVec2 push = { 0.0f, 0.0f };
        for (int i = 0; i < g_tracked_count; i++) {
            const Tracked *o = &g_tracked[i];
            if (o == self || now_s - o->last_t > FRESH_SECONDS) continue;
            AiVec2 dv = { zpos.x - o->pos.x, zpos.y - o->pos.y };
            float dist = len2(dv);
            if (dist >= reach || dist < 1e-5f) continue;
            float k = (1.0f - dist / reach) / dist;
            push.x += dv.x * k;
            push.y += dv.y * k;
        }
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

    /* Place the target a full chase distance away so Zombie::move keeps
     * full speed (it slows within one radius of its target and snaps). */
    AiVec2 target = { zpos.x + dir.x * d, zpos.y + dir.y * d };
    return target;
}
