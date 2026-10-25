#include "net_proto.h"
#include "lockstep.h"
#include "android_ndk.h"
#include "determinism.h"
#include "gun_hooks.h"
#include "watchdog.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Inputs are kept per frame in two rings, this PC's and the other's. The
 * input sampled at the end of frame F applies on frame F + delay on both
 * PCs; frames before `delay` have no input. At the end of each frame the
 * game thread sends its input and waits until the other side's input for
 * the next frame has arrived, so neither world can run ahead.
 *
 * Player 1's input is the host's (sticks and the touch / key events that
 * also drive the menus); player 2's is the guest's sticks. */

#define RING            512
#define MAX_DELAY       30
#define DEFAULT_DELAY   4       /* 67 ms: enough for most home connections */
#define WAIT_NOTICE_MS  3000
#define HASH_RING       16
#define SAVE_DIR_GUEST  "saves_lockstep"
#define MAX_SAVE_FILE   (4u * 1024u * 1024u)

typedef struct {
    uint32_t frame;
    int ready;
    float sticks[4];
    uint8_t fresh;
    uint8_t event_count;
    uint8_t *events;            /* LS_MAX_EVENTS * event size */
} Slot;

typedef struct {
    uint32_t frame;
    int have_mine, have_theirs;
    MsgLsHash mine, theirs;
} HashPair;

static LsRole g_role = LS_OFF;
static SOCKET g_sock = INVALID_SOCKET;
static int g_delay = DEFAULT_DELAY;
static size_t g_event_size = 0;
static Slot g_local[RING], g_remote[RING];
static CRITICAL_SECTION g_lock;          /* g_remote, g_hashes, g_peer_gone */
static CRITICAL_SECTION g_send_lock;
static HANDLE g_remote_event;
static volatile LONG g_peer_gone = 0;
static volatile LONG g_waiting = 0;
static volatile LONG g_desync = 0;
static HashPair g_hashes[HASH_RING];
static uint32_t g_in_sync_reported = 0;
static uint8_t *g_drain = NULL;          /* host: events drained from the queue */

/* Event delivery within the current frame */
static uint32_t g_take_frame = UINT32_MAX;
static int g_take_index = 0;

void lockstep_config_default(LockstepConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = NET_DEFAULT_PORT;
    cfg->delay = DEFAULT_DELAY;
}

int lockstep_active(void) { return g_role != LS_OFF; }
int lockstep_is_guest(void) { return g_role == LS_GUEST; }
int lockstep_peer_present(void) { return g_role != LS_OFF && !g_peer_gone; }
int lockstep_out_of_sync(void) { return g_desync != 0; }
int lockstep_waiting(void) { return g_waiting != 0; }

const char *lockstep_data_path(void) {
    return g_role == LS_GUEST ? SAVE_DIR_GUEST "/" : NULL;
}

static int send_msg(uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    if (g_sock == INVALID_SOCKET || g_peer_gone) return -1;
    EnterCriticalSection(&g_send_lock);
    int r = net_send_msg(g_sock, type, a, alen, b, blen);
    LeaveCriticalSection(&g_send_lock);
    return r;
}

static void peer_left(const char *why) {
    if (InterlockedExchange(&g_peer_gone, 1)) return;
    printf("[Lockstep] %s left (%s) - carrying on alone\n", g_role == LS_HOST ? "Player 2" : "The host", why);
    fflush(stdout);
    if (g_remote_event) SetEvent(g_remote_event);
}

/* ---------------------------------------------------------------- */
/* Handshake                                                        */
/* ---------------------------------------------------------------- */

static int send_saves(void) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("saves\\*", &fd);
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (strlen(fd.cFileName) >= LS_FILE_NAME_LEN || fd.nFileSizeLow > MAX_SAVE_FILE) continue;
            char path[MAX_PATH + 16];
            snprintf(path, sizeof(path), "saves\\%s", fd.cFileName);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            uint8_t *data = (uint8_t *)malloc(fd.nFileSizeLow + 1);
            size_t got = data ? fread(data, 1, fd.nFileSizeLow, f) : 0;
            fclose(f);
            char name[LS_FILE_NAME_LEN];
            memset(name, 0, sizeof(name));
            strcpy(name, fd.cFileName);
            int err = net_send_msg(g_sock, MSG_LS_FILE, name, sizeof(name), data, (uint32_t)got);
            free(data);
            if (err) {
                FindClose(h);
                return -1;
            }
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    printf("[Lockstep] Sent %d save files, so both games start the same\n", n);
    return 0;
}

static void clear_guest_saves(void) {
    CreateDirectoryA(SAVE_DIR_GUEST, NULL);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(SAVE_DIR_GUEST "\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH + 32];
        snprintf(path, sizeof(path), SAVE_DIR_GUEST "\\%s", fd.cFileName);
        DeleteFileA(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void reject(SOCKET c, const char *reason) {
    printf("[Lockstep] Refused a player: %s\n", reason);
    net_send_msg(c, MSG_REJECT, reason, (uint32_t)strlen(reason), NULL, 0);
    closesocket(c);
}

static int host_wait_for_guest(const LockstepConfig *cfg, const MsgLsSetup *setup, const char *build, uint32_t game_hash) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return -1;
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)cfg->port);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        printf("[Lockstep] Can't listen on port %d (already in use?)\n", cfg->port);
        closesocket(listener);
        return -1;
    }
    printf("[Lockstep] Waiting for player 2 on port %d - they run: run.bat --lockstep-join <this PC's IP>%s\n",
           cfg->port, cfg->code[0] ? " --code <code>" : "");
    fflush(stdout);

    for (;;) {
        struct sockaddr_in peer;
        int peer_len = sizeof(peer);
        SOCKET c = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (c == INVALID_SOCKET) continue;
        net_set_nodelay(c);

        uint8_t type, *buf = NULL;
        uint32_t len = 0;
        if (net_recv_msg(c, &type, &buf, &len) != 0) {
            closesocket(c);
            continue;
        }
        MsgLsHello hello;
        memset(&hello, 0, sizeof(hello));
        if (type == MSG_LS_HELLO && len >= sizeof(hello)) memcpy(&hello, buf, sizeof(hello));
        free(buf);

        if (type == MSG_HELLO) {
            reject(c, "this host plays own-world co-op: join with --lockstep-join");
            continue;
        }
        if (type != MSG_LS_HELLO || hello.version != LS_VERSION) {
            reject(c, "different co-op version - update both PCs (git pull)");
            continue;
        }
        hello.code[NET_CODE_LEN - 1] = '\0';
        if (cfg->code[0] && strcmp(cfg->code, hello.code) != 0) {
            reject(c, "wrong join code");
            continue;
        }
        if (hello.game_hash != game_hash || hello.event_size != g_event_size) {
            reject(c, "different game files - both PCs need the same libminigore2.so");
            continue;
        }
        hello.build[sizeof(hello.build) - 1] = '\0';
        if (strcmp(hello.build, build) != 0) {
            printf("[Lockstep] Note: player 2 runs build %s, this PC %s - if the worlds drift apart, update both\n",
                   hello.build, build);
        }

        g_sock = c;
        MsgLsHello mine = hello; /* echo = accepted */
        if (net_send_msg(c, MSG_LS_HELLO, &mine, sizeof(mine), NULL, 0) != 0 ||
            net_send_msg(c, MSG_LS_SETUP, setup, sizeof(*setup), NULL, 0) != 0 ||
            send_saves() != 0 ||
            net_send_msg(c, MSG_LS_START, NULL, 0, NULL, 0) != 0) {
            printf("[Lockstep] Player 2 dropped during setup; waiting again\n");
            closesocket(c);
            g_sock = INVALID_SOCKET;
            continue;
        }
        char ip[64];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        printf("[Lockstep] Player 2 joined from %s - starting\n", ip);
        closesocket(listener);
        return 0;
    }
}

static SOCKET connect_to(const char *host, int port) {
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port_s, &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) net_set_nodelay(s);
    return s;
}

static int guest_join(const LockstepConfig *cfg, MsgLsSetup *setup, const char *build, uint32_t game_hash) {
    for (;;) {
        printf("[Lockstep] Connecting to %s:%d...\n", cfg->host, cfg->port);
        fflush(stdout);
        SOCKET s = connect_to(cfg->host, cfg->port);
        if (s == INVALID_SOCKET) {
            printf("[Lockstep] Can't reach %s - retrying (is the host running with --lockstep-host, and the port open?)\n",
                   cfg->host);
            fflush(stdout);
            Sleep(3000);
            continue;
        }
        MsgLsHello hello;
        memset(&hello, 0, sizeof(hello));
        hello.version = LS_VERSION;
        strncpy(hello.code, cfg->code, NET_CODE_LEN - 1);
        strncpy(hello.build, build, sizeof(hello.build) - 1);
        hello.game_hash = game_hash;
        hello.event_size = (uint32_t)g_event_size;
        if (net_send_msg(s, MSG_LS_HELLO, &hello, sizeof(hello), NULL, 0) != 0) {
            closesocket(s);
            Sleep(2000);
            continue;
        }

        int have_setup = 0, files = 0, ok = 0;
        clear_guest_saves();
        for (;;) {
            uint8_t type, *buf = NULL;
            uint32_t len = 0;
            if (net_recv_msg(s, &type, &buf, &len) != 0) break;
            if (type == MSG_REJECT) {
                char reason[160];
                uint32_t n = len < sizeof(reason) - 1 ? len : (uint32_t)sizeof(reason) - 1;
                memcpy(reason, buf, n);
                reason[n] = '\0';
                printf("[Lockstep] Host refused: %s\n", reason);
                free(buf);
                closesocket(s);
                return -1;
            } else if (type == MSG_LS_SETUP && len >= sizeof(*setup)) {
                memcpy(setup, buf, sizeof(*setup));
                have_setup = 1;
            } else if (type == MSG_LS_FILE && len >= LS_FILE_NAME_LEN) {
                char name[LS_FILE_NAME_LEN];
                memcpy(name, buf, sizeof(name));
                name[LS_FILE_NAME_LEN - 1] = '\0';
                if (!strchr(name, '\\') && !strchr(name, '/') && !strstr(name, "..") && name[0]) {
                    char path[MAX_PATH + 32];
                    snprintf(path, sizeof(path), SAVE_DIR_GUEST "\\%s", name);
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        fwrite(buf + LS_FILE_NAME_LEN, 1, len - LS_FILE_NAME_LEN, f);
                        fclose(f);
                        files++;
                    }
                }
            } else if (type == MSG_LS_START) {
                ok = have_setup;
                free(buf);
                break;
            }
            free(buf);
        }
        if (ok) {
            g_sock = s;
            printf("[Lockstep] Joined: the host's saves (%d files) and settings received - starting\n", files);
            return 0;
        }
        closesocket(s);
        printf("[Lockstep] Lost the host during setup - retrying\n");
        Sleep(2000);
    }
}

/* ---------------------------------------------------------------- */
/* Receiving                                                        */
/* ---------------------------------------------------------------- */

static DWORD WINAPI receiver_thread(LPVOID unused) {
    for (;;) {
        uint8_t type, *buf = NULL;
        uint32_t len = 0;
        if (net_recv_msg(g_sock, &type, &buf, &len) != 0) {
            peer_left("connection lost");
            return 0;
        }
        if (type == MSG_LS_INPUT && len >= sizeof(MsgLsInput)) {
            MsgLsInput in;
            memcpy(&in, buf, sizeof(in));
            unsigned n = in.event_count;
            if (n > LS_MAX_EVENTS) n = LS_MAX_EVENTS;
            if (len < sizeof(in) + n * g_event_size) n = (unsigned)((len - sizeof(in)) / g_event_size);
            EnterCriticalSection(&g_lock);
            Slot *s = &g_remote[in.frame % RING];
            s->frame = in.frame;
            memcpy(s->sticks, in.sticks, sizeof(s->sticks));
            s->fresh = in.fresh;
            s->event_count = (uint8_t)n;
            if (n) memcpy(s->events, buf + sizeof(in), n * g_event_size);
            s->ready = 1;
            LeaveCriticalSection(&g_lock);
            SetEvent(g_remote_event);
        } else if (type == MSG_LS_HASH && len >= sizeof(MsgLsHash)) {
            MsgLsHash h;
            memcpy(&h, buf, sizeof(h));
            EnterCriticalSection(&g_lock);
            HashPair *p = &g_hashes[(h.frame / 60) % HASH_RING];
            if (p->frame != h.frame) {
                memset(p, 0, sizeof(*p));
                p->frame = h.frame;
            }
            p->theirs = h;
            p->have_theirs = 1;
            LeaveCriticalSection(&g_lock);
        } else if (type == MSG_LS_BYE) {
            free(buf);
            peer_left("quit");
            return 0;
        }
        free(buf);
    }
}

int lockstep_connect(const LockstepConfig *cfg, MsgLsSetup *setup, const char *build, uint32_t game_hash) {
    if (cfg->role == LS_OFF) return -1;
    if (net_startup() != 0) {
        printf("[Lockstep] Networking unavailable\n");
        return -1;
    }
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_send_lock);
    g_event_size = android_app_event_size();

    int r;
    if (cfg->role == LS_HOST) {
        g_delay = cfg->delay < 1 ? 1 : (cfg->delay > MAX_DELAY ? MAX_DELAY : cfg->delay);
        setup->delay = g_delay;
        r = host_wait_for_guest(cfg, setup, build, game_hash);
    } else {
        r = guest_join(cfg, setup, build, game_hash);
        g_delay = setup->delay < 1 ? 1 : (setup->delay > MAX_DELAY ? MAX_DELAY : setup->delay);
    }
    if (r != 0) return r;

    for (int i = 0; i < RING; i++) {
        g_local[i].events = (uint8_t *)calloc(LS_MAX_EVENTS, g_event_size);
        g_remote[i].events = (uint8_t *)calloc(LS_MAX_EVENTS, g_event_size);
        g_local[i].frame = g_remote[i].frame = UINT32_MAX;
    }
    g_drain = (uint8_t *)calloc(LS_MAX_EVENTS, g_event_size);
    g_remote_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_role = cfg->role;
    CreateThread(NULL, 0, receiver_thread, NULL, 0, NULL);
    printf("[Lockstep] Own-world co-op: you are player %d, input delay %d frames (%d ms)\n",
           g_role == LS_HOST ? 1 : 2, g_delay, g_delay * 1000 / 60);
    fflush(stdout);
    return 0;
}

void lockstep_shutdown(void) {
    if (g_role == LS_OFF || g_sock == INVALID_SOCKET) return;
    send_msg(MSG_LS_BYE, NULL, 0, NULL, 0);
    InterlockedExchange(&g_peer_gone, 1);
    shutdown(g_sock, SD_BOTH);
}

/* ---------------------------------------------------------------- */
/* Game thread                                                      */
/* ---------------------------------------------------------------- */

static void check_hashes(void) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < HASH_RING; i++) {
        HashPair *p = &g_hashes[i];
        if (!p->have_mine || !p->have_theirs) continue;
        int c = p->mine.characters == p->theirs.characters;
        int e = p->mine.entities == p->theirs.entities;
        int r = p->mine.rng == p->theirs.rng;
        uint32_t frame = p->frame;
        p->have_mine = p->have_theirs = 0;
        if (!(c && e && r)) {
            if (!InterlockedExchange(&g_desync, 1)) {
                printf("[Lockstep] OUT OF SYNC at frame %u (%.1f s): players %s, world %s, random numbers %s - "
                       "the two games have drifted apart\n", frame, frame / 60.0,
                       c ? "same" : "DIFFERENT", e ? "same" : "DIFFERENT", r ? "same" : "DIFFERENT");
                fflush(stdout);
            }
        } else if (frame >= g_in_sync_reported + 1800) {
            g_in_sync_reported = frame;
            printf("[Lockstep] Frame %u: both worlds identical%s\n", frame, g_desync ? " again" : "");
        }
    }
    LeaveCriticalSection(&g_lock);
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

void lockstep_frame_end(uint32_t next, const MsgLsHash *fp) {
    if (g_role == LS_OFF) return;
    pace_frame();

    /* This frame's fingerprint, compared once the other side's arrives */
    if (fp) {
        EnterCriticalSection(&g_lock);
        HashPair *p = &g_hashes[(fp->frame / 60) % HASH_RING];
        if (p->frame != fp->frame) {
            memset(p, 0, sizeof(*p));
            p->frame = fp->frame;
        }
        p->mine = *fp;
        p->have_mine = 1;
        LeaveCriticalSection(&g_lock);
        send_msg(MSG_LS_HASH, fp, sizeof(*fp), NULL, 0);
    }
    check_hashes();

    /* Sample this PC's input; it applies `delay` frames from now */
    uint32_t target = next - 1 + (uint32_t)g_delay;
    Slot *mine = &g_local[target % RING];
    mine->frame = target;
    mine->fresh = (uint8_t)gun_get_local_sticks(mine->sticks);
    int n = android_app_drain_events(g_drain, LS_MAX_EVENTS);
    if (g_role == LS_GUEST) n = 0; /* the host drives the menus */
    mine->event_count = (uint8_t)n;
    if (n) memcpy(mine->events, g_drain, (size_t)n * g_event_size);
    mine->ready = 1;

    MsgLsInput msg;
    msg.frame = target;
    memcpy(msg.sticks, mine->sticks, sizeof(msg.sticks));
    msg.fresh = mine->fresh;
    msg.event_count = mine->event_count;
    if (send_msg(MSG_LS_INPUT, &msg, sizeof(msg), mine->events, (uint32_t)n * (uint32_t)g_event_size) != 0 &&
        !g_peer_gone) {
        peer_left("send failed");
    }

    /* Wait for the other side's input for the next frame */
    if (next < (uint32_t)g_delay) return;
    DWORD start = GetTickCount();
    int noticed = 0;
    for (;;) {
        EnterCriticalSection(&g_lock);
        int ready = g_remote[next % RING].ready && g_remote[next % RING].frame == next;
        LeaveCriticalSection(&g_lock);
        if (ready || g_peer_gone) break;
        InterlockedExchange(&g_waiting, 1);
        WaitForSingleObject(g_remote_event, 50);
        watchdog_frame(); /* waiting for the network, not hung */
        if (!noticed && GetTickCount() - start > WAIT_NOTICE_MS) {
            noticed = 1;
            printf("[Lockstep] Waiting for the other player (slow connection or loading)...\n");
            fflush(stdout);
        }
    }
    InterlockedExchange(&g_waiting, 0);
}

/* This frame's slot for player 1 (host) or 2 (guest), or NULL */
static const Slot *slot_for(int player) {
    if (g_role == LS_OFF) return NULL;
    uint32_t frame = det_frame();
    int local = (player == 1) == (g_role == LS_HOST);
    if (local) {
        const Slot *s = &g_local[frame % RING];
        return s->ready && s->frame == frame ? s : NULL;
    }
    EnterCriticalSection(&g_lock);
    const Slot *s = &g_remote[frame % RING];
    int ok = s->ready && s->frame == frame;
    LeaveCriticalSection(&g_lock);
    return ok ? s : NULL;
}

int lockstep_p1_sticks(float sticks[4]) {
    const Slot *s = slot_for(1);
    if (!s) {
        memset(sticks, 0, 16);
        return 0;
    }
    memcpy(sticks, s->sticks, 16);
    return s->fresh;
}

int lockstep_p2_sticks(float sticks[4]) {
    memset(sticks, 0, 16);
    if (g_role == LS_OFF) return 0;
    /* After the other side leaves the worlds have parted anyway: the host
     * hands player 2 back to the companion AI */
    if (g_peer_gone && g_role == LS_HOST) return 0;
    const Slot *s = slot_for(2);
    if (s && s->fresh) memcpy(sticks, s->sticks, 16);
    return 1;
}

int lockstep_event_pending(void) {
    const Slot *s = slot_for(1);
    if (!s) return 0;
    uint32_t frame = det_frame();
    int index = g_take_frame == frame ? g_take_index : 0;
    return index < s->event_count;
}

int lockstep_take_event(void *event, size_t size) {
    const Slot *s = slot_for(1);
    if (!s || size != g_event_size) return 0;
    uint32_t frame = det_frame();
    if (g_take_frame != frame) {
        g_take_frame = frame;
        g_take_index = 0;
    }
    if (g_take_index >= s->event_count) return 0;
    memcpy(event, s->events + (size_t)g_take_index * g_event_size, size);
    g_take_index++;
    return 1;
}
