#include "net_proto.h"
#include "net_host.h"
#include <windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"

/* Co-op host
 *
 * Three background threads:
 *  - listener: accepts one guest at a time, checks the hello / join code,
 *    then receives the guest's input until the connection drops;
 *  - encoder:  turns captured frames into JPEG;
 *  - sender:   streams the newest JPEG (older unsent frames are dropped, so
 *              a slow link lowers the frame rate instead of adding lag) and
 *              the game audio.
 * Frames are captured on the game thread at swap time: the back buffer is
 * resolved (MSAA) and scaled down on the GPU with framebuffer blits, and
 * only the small image is read back. */

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER          0x8CA8
#define GL_DRAW_FRAMEBUFFER          0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING  0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING  0x8CA6
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER              0x8D41
#define GL_RENDERBUFFER_BINDING      0x8CA7
#define GL_COLOR_ATTACHMENT0         0x8CE0
#define GL_FRAMEBUFFER_COMPLETE      0x8CD5
#endif

typedef void (APIENTRY *PFN_GenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_DeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_BindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_GenRenderbuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_DeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_BindRenderbuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFN_CheckFramebufferStatus)(GLenum);
typedef void (APIENTRY *PFN_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

#define AUDIO_RING_BYTES (44100 * 4)  /* one second of s16 stereo */

static NetHostConfig g_cfg;
static volatile LONG g_active = 0;
static volatile LONG g_connected = 0;
static SOCKET g_listen = INVALID_SOCKET;
static SOCKET g_guest = INVALID_SOCKET;
static CRITICAL_SECTION g_lock;
static HANDLE g_send_event, g_encode_event;

/* Guest input (g_lock) */
static MsgInput g_input;
static DWORD g_input_tick = 0;
static int g_have_input = 0;

/* Player 2 state for the video header (g_lock) */
static MsgVideoHeader g_p2_state;

/* Capture -> encoder hand-off */
static uint8_t *g_capture = NULL;
static size_t g_capture_cap = 0;
static int g_cap_w = 0, g_cap_h = 0;
static volatile LONG g_encoder_busy = 0;
static DWORD g_last_capture = 0;

/* Encoder -> sender hand-off (g_lock) */
static uint8_t *g_jpeg = NULL;
static uint32_t g_jpeg_len = 0;
static MsgVideoHeader g_jpeg_header;
static int g_jpeg_ready = 0;

/* Audio ring (g_lock) */
static uint8_t g_audio[AUDIO_RING_BYTES];
static uint32_t g_audio_read = 0, g_audio_write = 0, g_audio_used = 0;

/* GL capture resources (game thread) */
static int g_gl_ready = 0, g_gl_failed = 0;
static PFN_GenFramebuffers gl_gen_fb;
static PFN_DeleteFramebuffers gl_del_fb;
static PFN_BindFramebuffer gl_bind_fb;
static PFN_GenRenderbuffers gl_gen_rb;
static PFN_DeleteRenderbuffers gl_del_rb;
static PFN_BindRenderbuffer gl_bind_rb;
static PFN_RenderbufferStorage gl_rb_storage;
static PFN_FramebufferRenderbuffer gl_fb_rb;
static PFN_CheckFramebufferStatus gl_check_fb;
static PFN_BlitFramebuffer gl_blit;
static GLuint g_fb_full, g_rb_full, g_fb_small, g_rb_small;
static int g_full_w, g_full_h, g_small_w, g_small_h;

void net_host_config_default(NetHostConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = NET_DEFAULT_PORT;
    cfg->width = 960;
    cfg->fps = 30;
    cfg->quality = 60;
}

int net_host_active(void) { return g_active != 0; }
int net_host_guest_connected(void) { return g_connected != 0; }

int net_host_get_input(MsgInput *out, unsigned max_age_ms) {
    int ok = 0;
    if (!g_connected) return 0;
    EnterCriticalSection(&g_lock);
    if (g_have_input && GetTickCount() - g_input_tick <= max_age_ms) {
        *out = g_input;
        ok = 1;
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

void net_host_set_p2(float x, float y, int alive, int in_level) {
    if (!g_active) return;
    EnterCriticalSection(&g_lock);
    g_p2_state.p2_x = x;
    g_p2_state.p2_y = y;
    g_p2_state.p2_alive = (uint8_t)(alive != 0);
    g_p2_state.in_level = (uint8_t)(in_level != 0);
    LeaveCriticalSection(&g_lock);
}

void net_host_audio(const void *pcm, int bytes) {
    if (!g_connected || bytes <= 0) return;
    const uint8_t *src = (const uint8_t *)pcm;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < bytes; i++) {
        g_audio[g_audio_write] = src[i];
        g_audio_write = (g_audio_write + 1) % AUDIO_RING_BYTES;
        if (g_audio_used < AUDIO_RING_BYTES) {
            g_audio_used++;
        } else {
            g_audio_read = (g_audio_read + 1) % AUDIO_RING_BYTES; /* overwrite oldest */
        }
    }
    LeaveCriticalSection(&g_lock);
    SetEvent(g_send_event);
}

/* ---------------------------------------------------------------- */
/* Frame capture (game thread)                                      */
/* ---------------------------------------------------------------- */

static int capture_gl_init(void) {
    if (g_gl_ready) return 1;
    if (g_gl_failed) return 0;
    gl_gen_fb = (PFN_GenFramebuffers)SDL_GL_GetProcAddress("glGenFramebuffers");
    gl_del_fb = (PFN_DeleteFramebuffers)SDL_GL_GetProcAddress("glDeleteFramebuffers");
    gl_bind_fb = (PFN_BindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
    gl_gen_rb = (PFN_GenRenderbuffers)SDL_GL_GetProcAddress("glGenRenderbuffers");
    gl_del_rb = (PFN_DeleteRenderbuffers)SDL_GL_GetProcAddress("glDeleteRenderbuffers");
    gl_bind_rb = (PFN_BindRenderbuffer)SDL_GL_GetProcAddress("glBindRenderbuffer");
    gl_rb_storage = (PFN_RenderbufferStorage)SDL_GL_GetProcAddress("glRenderbufferStorage");
    gl_fb_rb = (PFN_FramebufferRenderbuffer)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    gl_check_fb = (PFN_CheckFramebufferStatus)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    gl_blit = (PFN_BlitFramebuffer)SDL_GL_GetProcAddress("glBlitFramebuffer");
    if (!gl_blit) gl_blit = (PFN_BlitFramebuffer)SDL_GL_GetProcAddress("glBlitFramebufferEXT");
    if (!gl_gen_fb || !gl_bind_fb || !gl_gen_rb || !gl_bind_rb || !gl_rb_storage || !gl_fb_rb || !gl_blit || !gl_check_fb) {
        printf("[Co-op] Framebuffer blit not supported by this GPU driver; video streaming disabled\n");
        fflush(stdout);
        g_gl_failed = 1;
        return 0;
    }
    gl_gen_fb(1, &g_fb_full);
    gl_gen_fb(1, &g_fb_small);
    gl_gen_rb(1, &g_rb_full);
    gl_gen_rb(1, &g_rb_small);
    g_gl_ready = 1;
    return 1;
}

static int resize_target(GLuint fb, GLuint rb, int w, int h) {
    gl_bind_rb(GL_RENDERBUFFER, rb);
    gl_rb_storage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    gl_bind_fb(GL_DRAW_FRAMEBUFFER, fb);
    gl_fb_rb(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    return gl_check_fb(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void net_host_capture_frame(int width, int height) {
    if (!g_connected || g_encoder_busy || width <= 0 || height <= 0) return;
    DWORD now = GetTickCount();
    int fps = g_cfg.fps > 0 ? g_cfg.fps : 30;
    if (now - g_last_capture < (DWORD)(1000 / fps)) return;
    if (!capture_gl_init()) return;

    int tw = g_cfg.width < width ? g_cfg.width : width;
    tw &= ~1;
    int th = (int)((long long)tw * height / width) & ~1;
    if (tw < 64 || th < 64) return;

    GLint read_fb = 0, draw_fb = 0, rb = 0, pack = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    int ok = 1;
    if (g_full_w != width || g_full_h != height) {
        ok = resize_target(g_fb_full, g_rb_full, width, height);
        g_full_w = width;
        g_full_h = height;
    }
    if (ok && (g_small_w != tw || g_small_h != th)) {
        ok = resize_target(g_fb_small, g_rb_small, tw, th);
        g_small_w = tw;
        g_small_h = th;
    }

    if (ok) {
        glDisable(GL_SCISSOR_TEST); /* blits honour the scissor box */
        /* 1: resolve the (possibly multisampled) back buffer at full size */
        gl_bind_fb(GL_READ_FRAMEBUFFER, 0);
        gl_bind_fb(GL_DRAW_FRAMEBUFFER, g_fb_full);
        gl_blit(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        /* 2: scale down with filtering */
        gl_bind_fb(GL_READ_FRAMEBUFFER, g_fb_full);
        gl_bind_fb(GL_DRAW_FRAMEBUFFER, g_fb_small);
        gl_blit(0, 0, width, height, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        /* 3: read back the small image */
        size_t need = (size_t)tw * th * 4;
        if (need > g_capture_cap) {
            free(g_capture);
            g_capture = (uint8_t *)malloc(need);
            g_capture_cap = g_capture ? need : 0;
        }
        if (g_capture) {
            gl_bind_fb(GL_READ_FRAMEBUFFER, g_fb_small);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, g_capture);
        }
    } else {
        printf("[Co-op] Could not create capture framebuffers; video streaming disabled\n");
        fflush(stdout);
        g_gl_failed = 1;
        g_gl_ready = 0;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    gl_bind_rb(GL_RENDERBUFFER, (GLuint)rb);
    gl_bind_fb(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    gl_bind_fb(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);

    if (ok && g_capture) {
        g_cap_w = tw;
        g_cap_h = th;
        g_last_capture = now;
        InterlockedExchange(&g_encoder_busy, 1);
        SetEvent(g_encode_event);
    }
}

/* ---------------------------------------------------------------- */
/* Threads                                                          */
/* ---------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t len, cap;
} ByteBuf;

static void jpeg_write(void *ctx, void *data, int size) {
    ByteBuf *b = (ByteBuf *)ctx;
    if (b->len + (size_t)size > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->len + (size_t)size) cap *= 2;
        uint8_t *p = (uint8_t *)realloc(b->data, cap);
        if (!p) return;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, (size_t)size);
    b->len += (size_t)size;
}

static DWORD WINAPI encoder_thread(LPVOID unused) {
    uint8_t *rgb = NULL;
    size_t rgb_cap = 0;
    for (;;) {
        WaitForSingleObject(g_encode_event, INFINITE);
        if (!g_encoder_busy) continue;
        int w = g_cap_w, h = g_cap_h;
        size_t need = (size_t)w * h * 3;
        if (need > rgb_cap) {
            free(rgb);
            rgb = (uint8_t *)malloc(need);
            rgb_cap = rgb ? need : 0;
        }
        if (rgb) {
            /* GL rows are bottom-up RGBA; JPEG wants top-down RGB */
            for (int y = 0; y < h; y++) {
                const uint8_t *src = g_capture + (size_t)(h - 1 - y) * w * 4;
                uint8_t *dst = rgb + (size_t)y * w * 3;
                for (int x = 0; x < w; x++) {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                }
            }
        }
        InterlockedExchange(&g_encoder_busy, 0); /* capture buffer is free again */
        if (!rgb) continue;

        ByteBuf out = { NULL, 0, 0 };
        int quality = g_cfg.quality > 0 && g_cfg.quality <= 100 ? g_cfg.quality : 60;
        if (stbi_write_jpg_to_func(jpeg_write, &out, w, h, 3, rgb, quality) && out.len > 0) {
            EnterCriticalSection(&g_lock);
            free(g_jpeg); /* drop an older frame the sender hasn't taken */
            g_jpeg = out.data;
            g_jpeg_len = (uint32_t)out.len;
            g_jpeg_header = g_p2_state;
            g_jpeg_header.width = (uint16_t)w;
            g_jpeg_header.height = (uint16_t)h;
            g_jpeg_ready = 1;
            LeaveCriticalSection(&g_lock);
            SetEvent(g_send_event);
        } else {
            free(out.data);
        }
    }
    return 0;
}

static DWORD WINAPI sender_thread(LPVOID unused) {
    static uint8_t audio[AUDIO_RING_BYTES];
    for (;;) {
        WaitForSingleObject(g_send_event, 20);
        if (!g_connected) continue;
        SOCKET s = g_guest;
        if (s == INVALID_SOCKET) continue;

        uint32_t n = 0;
        uint8_t *jpeg = NULL;
        uint32_t jpeg_len = 0;
        MsgVideoHeader header;
        EnterCriticalSection(&g_lock);
        while (g_audio_used > 0) {
            audio[n++] = g_audio[g_audio_read];
            g_audio_read = (g_audio_read + 1) % AUDIO_RING_BYTES;
            g_audio_used--;
        }
        n &= ~3u; /* whole stereo frames */
        if (g_jpeg_ready) {
            jpeg = g_jpeg;
            jpeg_len = g_jpeg_len;
            header = g_jpeg_header;
            g_jpeg = NULL;
            g_jpeg_ready = 0;
        }
        LeaveCriticalSection(&g_lock);

        int err = 0;
        if (n) err = net_send_msg(s, MSG_AUDIO, audio, n, NULL, 0);
        if (!err && jpeg) err = net_send_msg(s, MSG_VIDEO, &header, sizeof(header), jpeg, jpeg_len);
        free(jpeg);
        if (err) shutdown(s, SD_BOTH); /* the listener thread cleans up */
    }
    return 0;
}

static float clamp_unit(float v) {
    if (!(v == v)) return 0.0f; /* NaN */
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

static DWORD WINAPI listener_thread(LPVOID unused) {
    for (;;) {
        struct sockaddr_in peer;
        int peer_len = sizeof(peer);
        SOCKET c = accept(g_listen, (struct sockaddr *)&peer, &peer_len);
        if (c == INVALID_SOCKET) {
            Sleep(200);
            continue;
        }
        net_set_nodelay(c);
        DWORD timeout = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

        uint8_t type = 0;
        uint8_t *buf = NULL;
        uint32_t len = 0;
        const char *reject = NULL;
        if (net_recv_msg(c, &type, &buf, &len) != 0 || type != MSG_HELLO || len < sizeof(MsgHello)) {
            reject = "bad handshake";
        } else {
            MsgHello *hello = (MsgHello *)buf;
            hello->code[NET_CODE_LEN - 1] = '\0';
            if (hello->version != NET_VERSION) reject = "version mismatch: update both games";
            else if (g_cfg.code[0] && strcmp(hello->code, g_cfg.code) != 0) reject = "wrong join code";
        }
        free(buf);
        if (reject) {
            net_send_msg(c, MSG_REJECT, reject, (uint32_t)strlen(reject), NULL, 0);
            printf("[Co-op] Rejected %s: %s\n", inet_ntoa(peer.sin_addr), reject);
            fflush(stdout);
            closesocket(c);
            continue;
        }

        MsgHello reply;
        memset(&reply, 0, sizeof(reply));
        reply.version = NET_VERSION;
        if (net_send_msg(c, MSG_HELLO, &reply, sizeof(reply), NULL, 0) != 0) {
            closesocket(c);
            continue;
        }
        /* Guests send input ~60 times a second; silence means they're gone */
        timeout = 10000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

        EnterCriticalSection(&g_lock);
        g_guest = c;
        g_have_input = 0;
        g_audio_read = g_audio_write = g_audio_used = 0;
        LeaveCriticalSection(&g_lock);
        InterlockedExchange(&g_connected, 1);
        printf("[Co-op] Player 2 joined from %s\n", inet_ntoa(peer.sin_addr));
        fflush(stdout);

        for (;;) {
            if (net_recv_msg(c, &type, &buf, &len) != 0) break;
            if (type == MSG_INPUT && len >= sizeof(MsgInput)) {
                MsgInput in;
                memcpy(&in, buf, sizeof(in));
                in.move_x = clamp_unit(in.move_x);
                in.move_y = clamp_unit(in.move_y);
                in.aim_x = clamp_unit(in.aim_x);
                in.aim_y = clamp_unit(in.aim_y);
                EnterCriticalSection(&g_lock);
                g_input = in;
                g_input_tick = GetTickCount();
                g_have_input = 1;
                LeaveCriticalSection(&g_lock);
            }
            free(buf);
        }

        InterlockedExchange(&g_connected, 0);
        EnterCriticalSection(&g_lock);
        g_guest = INVALID_SOCKET;
        g_have_input = 0;
        LeaveCriticalSection(&g_lock);
        shutdown(c, SD_BOTH);
        closesocket(c);
        printf("[Co-op] Player 2 disconnected; waiting for a new guest\n");
        fflush(stdout);
    }
    return 0;
}

static void print_local_addresses(int port) {
    char name[256];
    if (gethostname(name, sizeof(name)) != 0) return;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) != 0) return;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        struct sockaddr_in *sin = (struct sockaddr_in *)a->ai_addr;
        printf("[Co-op]   friends on your network: --join %s:%d\n", inet_ntoa(sin->sin_addr), port);
    }
    freeaddrinfo(res);
}

int net_host_start(const NetHostConfig *cfg) {
    g_cfg = *cfg;
    if (net_startup() != 0) {
        printf("[Co-op] Winsock failed to start\n");
        return -1;
    }
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return -1;
    BOOL reuse = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)cfg->port);
    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(g_listen, 1) != 0) {
        printf("[Co-op] Could not listen on port %d (error %d)\n", cfg->port, WSAGetLastError());
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return -1;
    }

    InitializeCriticalSection(&g_lock);
    g_send_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_encode_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, listener_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, encoder_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, sender_thread, NULL, 0, NULL));
    InterlockedExchange(&g_active, 1);

    printf("[Co-op] Hosting on port %d%s; streaming %dpx @ %d fps, JPEG quality %d\n",
           cfg->port, cfg->code[0] ? " (join code required)" : "", cfg->width, cfg->fps, cfg->quality);
    print_local_addresses(cfg->port);
    printf("[Co-op]   over the internet: forward TCP port %d to this PC, or use a LAN VPN (Tailscale, ZeroTier)\n",
           cfg->port);
    fflush(stdout);
    return 0;
}
