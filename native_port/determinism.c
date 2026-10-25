#include "determinism.h"
#include "shims.h"
#include "lockstep.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Recording format: header, then records of
 *   u8 kind, u32 frame, payload
 * kinds: STICKS (4 floats as written to the game's stick state after
 * StateGame::updateControls), EVENT (one Android input event the game
 * consumed), HASH (state fingerprint at the end of the frame), END. */

#define DET_MAGIC        "MG2DET1"
#define DET_VERSION      2   /* 2: g_random pinned and fingerprinted */
#define FRAME_NS_NUM     1000000000ULL      /* 1/60 s per frame, without drift */
#define FRAME_NS_DEN     60ULL
#define CLOCK_START_NS   (1000ULL * 1000000000ULL)
#define FIXED_WALL_TIME  1767225600LL       /* 2026-01-01 00:00:00 UTC */
#define FIXED_RANDOM_SEED 0x2545u           /* g_random, normally seeded from clock() */
#define MAX_ENTITIES     2048
#define MAX_REPORTS      10

enum { REC_STICKS = 1, REC_EVENT = 2, REC_HASH = 3, REC_END = 4, REC_SCREEN = 5 };

typedef struct {
    uint64_t characters, entities, rng;
    uint32_t entity_count, character_count;
} Fingerprint;

typedef struct {
    uint32_t frame;
    float sticks[4];
} StickRec;

typedef struct {
    uint32_t frame;
    uint32_t offset;  /* into g_event_bytes */
} EventRec;

typedef struct {
    uint32_t frame;
    Fingerprint fp;
} HashRec;

static DetMode g_mode = DET_OFF;
static char g_path[MAX_PATH];  /* recording file */
static char g_snapshot[MAX_PATH + 16];
static uint32_t g_frame = 0;
static uint64_t g_sleep_ns = 0;
static DWORD g_game_thread = 0;

/* Recording */
static FILE *g_out = NULL;
static size_t g_event_size = 0;

/* Replay */
static StickRec *g_sticks = NULL;
static size_t g_stick_count = 0, g_stick_cur = 0;
static EventRec *g_events = NULL;
static uint8_t *g_event_bytes = NULL;
static size_t g_event_count = 0, g_event_cur = 0;
static HashRec *g_hashes = NULL;
static size_t g_hash_count = 0, g_hash_cur = 0;
static uint32_t g_compared = 0, g_mismatches = 0, g_first_desync = 0;
static int g_reported_end = 0;
static int32_t g_screen[2] = { 0, 0 };   /* window size the recording was made at */

/* Game access for fingerprints */
static uint32_t *g_random = NULL;   /* the game's own LCG (spawns, pickups, patrols) */
#define ENV_CHARACTERS       0x1f4f1c
#define ENV_CHARACTER_COUNT  0x1f4f9c
#define ENV_ENTITY_SYSTEM    0x12784
#define ENTITY_POS           0x04
typedef int (__attribute__((cdecl)) *IntGetterFn)(const void *self);
typedef unsigned (__attribute__((cdecl)) *QueryFn)(void *system, const float *pos, float radius,
                                                   unsigned long long mask, unsigned flags,
                                                   void *results, unsigned max_results);
static uint8_t **g_env = NULL;
static uintptr_t g_base = 0;
static IntGetterFn g_get_health = NULL;
static QueryFn g_query = NULL;

void det_configure(DetMode mode, const char *path) {
    g_mode = mode;
    snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
    snprintf(g_snapshot, sizeof(g_snapshot), "%s.saves", g_path);
}

int det_active(void) { return g_mode != DET_OFF; }
int det_replaying(void) { return g_mode == DET_REPLAY; }

/* ---------------------------------------------------------------- */
/* Save snapshot                                                    */
/* ---------------------------------------------------------------- */

static void clear_dir(const char *dir) {
    char pattern[MAX_PATH + 8];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char file[2 * MAX_PATH];
        snprintf(file, sizeof(file), "%s\\%s", dir, fd.cFileName);
        DeleteFileA(file);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int copy_dir(const char *from, const char *to) {
    CreateDirectoryA(to, NULL);
    clear_dir(to);
    char pattern[MAX_PATH + 8];
    snprintf(pattern, sizeof(pattern), "%s\\*", from);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char a[2 * MAX_PATH], b[2 * MAX_PATH];
        snprintf(a, sizeof(a), "%s\\%s", from, fd.cFileName);
        snprintf(b, sizeof(b), "%s\\%s", to, fd.cFileName);
        if (CopyFileA(a, b, FALSE)) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

const char *det_prepare_saves(void) {
    if (g_mode == DET_OFF) return NULL;
    if (g_mode == DET_RECORD) {
        int n = copy_dir("saves", g_snapshot);
        printf("[Determinism] Save snapshot: %d files from saves\\ -> %s\\\n", n, g_snapshot);
    } else if (GetFileAttributesA(g_snapshot) == INVALID_FILE_ATTRIBUTES) {
        printf("[Determinism] Save snapshot %s missing; replay may desync immediately\n", g_snapshot);
    }
    /* Both runs play from a fresh copy of the snapshot, never the real saves */
    copy_dir(g_snapshot, "saves_determinism");
    return "saves_determinism/";
}

/* ---------------------------------------------------------------- */
/* Clock                                                            */
/* ---------------------------------------------------------------- */

uint64_t det_now_ns(void) {
    return CLOCK_START_NS + (uint64_t)g_frame * FRAME_NS_NUM / FRAME_NS_DEN + g_sleep_ns;
}

void det_advance_ns(uint64_t ns) {
    /* Only the game thread's sleeps move time (they are part of its
     * deterministic control flow); before the first frame any thread may. */
    if (!g_game_thread || GetCurrentThreadId() == g_game_thread) g_sleep_ns += ns;
}

/* The game loop (GameLoopStaticFPS) polls clock_gettime until a frame
 * step has elapsed, without sleeping. Each pass that drew nothing moves
 * the virtual clock a fixed 1 ms, or the loop would wait forever. */
void det_idle(void) {
    if (g_mode != DET_OFF) det_advance_ns(1000000ULL);
}

uint32_t det_frame(void) {
    return g_frame;
}

void det_rng_save(DetRng *r) {
    r->rand48 = *shims_rand48_state();
    r->random = g_random ? *g_random : 0;
}

void det_rng_restore(const DetRng *r) {
    *(volatile long long *)shims_rand48_state() = r->rand48;
    if (g_random) *g_random = r->random;
}

double det_sim_seconds(void) {
    return (double)det_now_ns() / 1e9;
}

int64_t det_wall_time(void) {
    return FIXED_WALL_TIME + (int64_t)(g_frame / 60);
}

/* ---------------------------------------------------------------- */
/* Fingerprint                                                      */
/* ---------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

typedef struct {
    uint8_t *entity;
    float value;
} QueryResult;

static void fingerprint(Fingerprint *fp) {
    memset(fp, 0, sizeof(*fp));
    uint64_t seed = 0xcbf29ce484222325ULL;
    fp->rng = fnv(seed, (const void *)shims_rand48_state(), 8);
    if (g_random) fp->rng = fnv(fp->rng, g_random, 4);

    uint8_t *e = g_env ? *g_env : NULL;
    if (!e) return;

    /* Player-side characters: position and health */
    uint64_t h = seed;
    unsigned count = *(const unsigned *)(e + ENV_CHARACTER_COUNT);
    if (count > 32) count = 32;
    uint8_t **list = (uint8_t **)(e + ENV_CHARACTERS);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *c = list[i];
        if (!c) continue;
        h = fnv(h, c + ENTITY_POS, 8);
        if (g_get_health) {
            int hp = g_get_health(c);
            h = fnv(h, &hp, sizeof(hp));
        }
    }
    fp->characters = h;
    fp->character_count = count;

    /* Every entity in the world: type (vtable relative to the module, so
     * independent of where it was loaded) and exact position */
    if (g_query) {
        static QueryResult results[MAX_ENTITIES];
        const float origin[2] = { 0.0f, 0.0f };
        unsigned n = g_query(e + ENV_ENTITY_SYSTEM, origin, 1.0e7f, ~0ULL, 0, results, MAX_ENTITIES);
        if (n > MAX_ENTITIES) n = MAX_ENTITIES;
        h = seed;
        for (unsigned i = 0; i < n; i++) {
            uint8_t *ent = results[i].entity;
            if (!ent) continue;
            uint32_t type = (uint32_t)(*(uintptr_t *)ent - g_base);
            h = fnv(h, &type, sizeof(type));
            h = fnv(h, ent + ENTITY_POS, 8);
        }
        fp->entities = h;
        fp->entity_count = n;
    }
}

/* ---------------------------------------------------------------- */
/* Recording / replay                                               */
/* ---------------------------------------------------------------- */

static void write_rec(uint8_t kind, const void *payload, size_t n) {
    if (!g_out) return;
    fwrite(&kind, 1, 1, g_out);
    fwrite(&g_frame, 4, 1, g_out);
    if (n) fwrite(payload, 1, n, g_out);
}

static int load_replay(void) {
    FILE *f = fopen(g_path, "rb");
    if (!f) {
        printf("[Determinism] Cannot open %s\n", g_path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return 0;
    }
    fclose(f);

    if (size < 16 || memcmp(data, DET_MAGIC, 8) != 0) {
        printf("[Determinism] %s is not a recording\n", g_path);
        free(data);
        return 0;
    }
    uint32_t version, event_size;
    memcpy(&version, data + 8, 4);
    memcpy(&event_size, data + 12, 4);
    if (version != DET_VERSION) {
        printf("[Determinism] Recording version %u, expected %u\n", version, DET_VERSION);
        free(data);
        return 0;
    }
    g_event_size = event_size;

    /* Two passes: count, then fill */
    for (int pass = 0; pass < 2; pass++) {
        size_t pos = 16, ns = 0, ne = 0, nh = 0, eb = 0;
        while (pos + 5 <= (size_t)size) {
            uint8_t kind = data[pos];
            uint32_t frame;
            memcpy(&frame, data + pos + 1, 4);
            pos += 5;
            if (kind == REC_STICKS && pos + 16 <= (size_t)size) {
                if (pass) { g_sticks[ns].frame = frame; memcpy(g_sticks[ns].sticks, data + pos, 16); }
                ns++; pos += 16;
            } else if (kind == REC_EVENT && pos + event_size <= (size_t)size) {
                if (pass) {
                    g_events[ne].frame = frame;
                    g_events[ne].offset = (uint32_t)eb;
                    memcpy(g_event_bytes + eb, data + pos, event_size);
                }
                ne++; eb += event_size; pos += event_size;
            } else if (kind == REC_SCREEN && pos + 8 <= (size_t)size) {
                if (pass) memcpy(g_screen, data + pos, 8);
                pos += 8;
            } else if (kind == REC_HASH && pos + sizeof(Fingerprint) <= (size_t)size) {
                if (pass) { g_hashes[nh].frame = frame; memcpy(&g_hashes[nh].fp, data + pos, sizeof(Fingerprint)); }
                nh++; pos += sizeof(Fingerprint);
            } else {
                break; /* REC_END or truncated */
            }
        }
        if (!pass) {
            g_sticks = (StickRec *)calloc(ns + 1, sizeof(StickRec));
            g_events = (EventRec *)calloc(ne + 1, sizeof(EventRec));
            g_event_bytes = (uint8_t *)malloc(eb + 1);
            g_hashes = (HashRec *)calloc(nh + 1, sizeof(HashRec));
        } else {
            g_stick_count = ns;
            g_event_count = ne;
            g_hash_count = nh;
        }
    }
    free(data);
    printf("[Determinism] Replaying %s: %zu frames, %zu input events, %zu stick updates\n",
           g_path, g_hash_count, g_event_count, g_stick_count);
    return 1;
}

void det_install(elf_module_t *mod) {
    if (g_mode == DET_OFF) return;
    g_env = (uint8_t **)elf_sym(mod, "m_env");
    g_base = (uintptr_t)mod->base;
    g_get_health = (IntGetterFn)elf_sym(mod, "_ZNK9Character9getHealthEv");
    g_query = (QueryFn)elf_sym(mod, "_ZN12EntitySystem19queryEntitiesAroundERKN2xt7Vector2IfEEfyjPNS_11QueryResultEj");

    /* A static constructor seeds g_random from clock() (CPU time so far),
     * different every launch; it decides zombie types, spawn spots, script
     * patterns and pickups. Constructors have run by now: pin it. */
    g_random = (uint32_t *)elf_sym(mod, "g_random");
    if (g_random) *g_random = FIXED_RANDOM_SEED;
    else printf("[Determinism] WARNING: g_random not found; spawns will differ\n");

    if (g_mode == DET_RECORD) {
        g_out = fopen(g_path, "wb");
        if (!g_out) {
            printf("[Determinism] Cannot write %s; recording disabled\n", g_path);
            g_mode = DET_OFF;
            return;
        }
        uint32_t version = DET_VERSION, event_size = 0; /* set by the first event */
        fwrite(DET_MAGIC, 1, 8, g_out);
        fwrite(&version, 4, 1, g_out);
        fwrite(&event_size, 4, 1, g_out);
        printf("[Determinism] Recording to %s - play normally, then quit\n", g_path);
    } else if (g_mode == DET_LOCKSTEP) {
        /* own-world co-op: same clock, seeds and fingerprints; inputs from lockstep.c */
    } else if (!load_replay()) {
        printf("[Determinism] Replay unavailable\n");
        g_mode = DET_OFF;
    } else {
        printf("[Determinism] Don't touch the controls: the recording is playing\n");
    }
}

void det_record_event(const void *event, size_t size) {
    if (g_mode != DET_RECORD || !g_out) return;
    if (!g_event_size) {
        /* patch the header now the event size is known */
        g_event_size = size;
        long here = ftell(g_out);
        uint32_t es = (uint32_t)size;
        fseek(g_out, 12, SEEK_SET);
        fwrite(&es, 4, 1, g_out);
        fseek(g_out, here, SEEK_SET);
    }
    write_rec(REC_EVENT, event, size);
}

int det_replay_pending(void) {
    if (g_mode != DET_REPLAY) return 0;
    size_t i = g_event_cur;
    while (i < g_event_count && g_events[i].frame < g_frame) i++;
    return i < g_event_count && g_events[i].frame == g_frame;
}

int det_replay_event(void *event, size_t size) {
    if (g_mode != DET_REPLAY) return 0;
    while (g_event_cur < g_event_count && g_events[g_event_cur].frame < g_frame) g_event_cur++;
    if (g_event_cur >= g_event_count || g_events[g_event_cur].frame != g_frame) return 0;
    if (size != g_event_size) return 0;
    memcpy(event, g_event_bytes + g_events[g_event_cur].offset, size);
    g_event_cur++;
    return 1;
}

void det_sticks(float sticks[4]) {
    if (g_mode == DET_RECORD) {
        write_rec(REC_STICKS, sticks, 16);
    } else if (g_mode == DET_REPLAY) {
        while (g_stick_cur < g_stick_count && g_sticks[g_stick_cur].frame < g_frame) g_stick_cur++;
        if (g_stick_cur < g_stick_count && g_sticks[g_stick_cur].frame == g_frame) {
            memcpy(sticks, g_sticks[g_stick_cur].sticks, 16);
            g_stick_cur++;
        } else {
            memset(sticks, 0, 16);
        }
    }
}

static const char *ok(int same) { return same ? "same" : "DIFFERENT"; }

void det_set_screen(int w, int h) {
    if (g_mode == DET_RECORD) {
        int32_t wh[2] = { w, h };
        write_rec(REC_SCREEN, wh, 8);
    } else if (g_mode == DET_REPLAY && g_screen[0] && (g_screen[0] != w || g_screen[1] != h)) {
        printf("[Determinism] WARNING: recorded at %dx%d but replaying at %dx%d - use the same window size "
               "(e.g. --width %d --height %d) or touches will land in different places\n",
               g_screen[0], g_screen[1], w, h, g_screen[0], g_screen[1]);
    }
}

/* The virtual clock advances 1/60 s per frame whatever the real rate, so
 * hold each frame to real time too (VSync alone runs fast on 120/144 Hz
 * screens or with VSync off). Deadlines are absolute, without drift; after
 * a stall (loading, waiting for the network) we restart rather than catch up. */
static void pace_frame(void) {
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    LONGLONG period = freq.QuadPart / 60;
    if (!next.QuadPart || now.QuadPart - next.QuadPart > period * 4) next = now;
    LONGLONG wait = next.QuadPart - now.QuadPart;
    if (wait > 0) Sleep((DWORD)(wait * 1000 / freq.QuadPart)); /* 1 ms timer resolution: shims.c */
    next.QuadPart += period;
}

void det_frame_end(void) {
    if (g_mode == DET_OFF) return;
    if (!g_game_thread) g_game_thread = GetCurrentThreadId();
    if (g_mode != DET_REPLAY) pace_frame(); /* replay runs unattended, as fast as it can */

    if (g_mode == DET_LOCKSTEP) {
        /* Compare worlds once a second; the frame ends when both players'
         * inputs for the next one are in */
        MsgLsHash h, *have = NULL;
        if (g_frame % 60 == 0) {
            Fingerprint f;
            fingerprint(&f);
            h.frame = g_frame;
            h.characters = f.characters;
            h.entities = f.entities;
            h.rng = f.rng;
            have = &h;
        }
        g_frame++;
        lockstep_frame_end(g_frame, have);
        return;
    }

    Fingerprint fp;
    fingerprint(&fp);

    if (g_mode == DET_RECORD) {
        write_rec(REC_HASH, &fp, sizeof(fp));
        if (g_frame % 600 == 0) {
            fflush(g_out);
            if (g_frame) printf("[Determinism] Recorded %u frames\n", g_frame);
        }
    } else {
        while (g_hash_cur < g_hash_count && g_hashes[g_hash_cur].frame < g_frame) g_hash_cur++;
        if (g_hash_cur < g_hash_count && g_hashes[g_hash_cur].frame == g_frame) {
            const Fingerprint *want = &g_hashes[g_hash_cur].fp;
            int c = want->characters == fp.characters;
            int en = want->entities == fp.entities && want->entity_count == fp.entity_count;
            int r = want->rng == fp.rng;
            g_compared++;
            if (!(c && en && r)) {
                if (!g_mismatches) g_first_desync = g_frame;
                if (++g_mismatches <= MAX_REPORTS) {
                    printf("[Determinism] DESYNC at frame %u: characters %s, entities %s (%u vs %u), random numbers %s\n",
                           g_frame, ok(c), ok(en), fp.entity_count, want->entity_count, ok(r));
                }
            } else if (g_frame % 600 == 0 && g_frame) {
                printf("[Determinism] Frame %u: %s\n", g_frame,
                       g_mismatches ? "matches again" : "in sync so far");
            }
        } else if (!g_reported_end && g_hash_cur >= g_hash_count) {
            g_reported_end = 1;
            det_finish();
        }
    }
    g_frame++;
}

void det_finish(void) {
    if (g_mode == DET_RECORD && g_out) {
        write_rec(REC_END, NULL, 0);
        fclose(g_out);
        g_out = NULL;
        printf("[Determinism] Recording saved: %u frames in %s (+ save snapshot %s)\n", g_frame, g_path, g_snapshot);
        printf("[Determinism] Now run: run.bat --replay %s\n", g_path);
    } else if (g_mode == DET_REPLAY) {
        if (g_mismatches == 0) {
            printf("[Determinism] RESULT: IN SYNC - all %u recorded frames matched exactly\n", g_compared);
        } else {
            printf("[Determinism] RESULT: DESYNC - first at frame %u (%.1f s in), %u of %u frames differed\n",
                   g_first_desync, g_first_desync / 60.0, g_mismatches, g_compared);
        }
    }
    fflush(stdout);
}
