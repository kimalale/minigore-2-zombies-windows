#include "net_proto.h"
#include "guest.h"
#include <windows.h>
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

/* Co-op guest: connects to a host, shows its video and plays its audio,
 * and sends player 2's controls ~60 times a second. Reconnects on its
 * own if the link drops. */

#define PAD_DEADZONE      0.22f
#define TRIGGER_FIRE      0.35f
#define MAX_AUDIO_LAG     (44100 * 4 * 3 / 10)  /* drop queued audio beyond 300 ms */

static GuestConfig g_cfg;
static CRITICAL_SECTION g_lock;
static volatile SOCKET g_sock = INVALID_SOCKET;
static char g_status[512] = "Connecting...";

/* Latest decoded frame (g_lock) */
static uint8_t *g_frame = NULL;
static int g_frame_w = 0, g_frame_h = 0, g_frame_new = 0;
static MsgVideoHeader g_frame_info;

static SDL_AudioDeviceID g_audio = 0;

static void set_status(const char *fmt, const char *arg) {
    EnterCriticalSection(&g_lock);
    snprintf(g_status, sizeof(g_status), fmt, arg);
    LeaveCriticalSection(&g_lock);
}

static SOCKET connect_to_host(void) {
    char port[16];
    snprintf(port, sizeof(port), "%d", g_cfg.port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(g_cfg.host, port, &hints, &res) != 0) return INVALID_SOCKET;
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

static DWORD WINAPI connection_thread(LPVOID unused) {
    for (;;) {
        set_status("Connecting to %s...", g_cfg.host);
        SOCKET s = connect_to_host();
        if (s == INVALID_SOCKET) {
            set_status("Can't reach %s - retrying (is the host running with --host, and the port open?)", g_cfg.host);
            Sleep(3000);
            continue;
        }

        MsgHello hello;
        memset(&hello, 0, sizeof(hello));
        hello.version = NET_VERSION;
        strncpy(hello.code, g_cfg.code, NET_CODE_LEN - 1);
        if (net_send_msg(s, MSG_HELLO, &hello, sizeof(hello), NULL, 0) != 0) {
            closesocket(s);
            Sleep(2000);
            continue;
        }

        int rejected = 0;
        for (;;) {
            uint8_t type;
            uint8_t *buf;
            uint32_t len;
            if (net_recv_msg(s, &type, &buf, &len) != 0) break;

            if (type == MSG_HELLO) {
                g_sock = s;
                set_status("Connected to %s - waiting for the host to start a level", g_cfg.host);
            } else if (type == MSG_REJECT) {
                char reason[128];
                uint32_t n = len < sizeof(reason) - 1 ? len : (uint32_t)sizeof(reason) - 1;
                memcpy(reason, buf, n);
                reason[n] = '\0';
                set_status("Host refused the connection: %s", reason);
                rejected = 1;
                free(buf);
                break;
            } else if (type == MSG_VIDEO && len > sizeof(MsgVideoHeader)) {
                MsgVideoHeader info;
                memcpy(&info, buf, sizeof(info));
                int w, h, n;
                uint8_t *pixels = stbi_load_from_memory(buf + sizeof(info), (int)(len - sizeof(info)), &w, &h, &n, 3);
                if (pixels) {
                    EnterCriticalSection(&g_lock);
                    stbi_image_free(g_frame);
                    g_frame = pixels;
                    g_frame_w = w;
                    g_frame_h = h;
                    g_frame_info = info;
                    g_frame_new = 1;
                    LeaveCriticalSection(&g_lock);
                }
            } else if (type == MSG_AUDIO && g_audio) {
                if (SDL_GetQueuedAudioSize(g_audio) > MAX_AUDIO_LAG) SDL_ClearQueuedAudio(g_audio);
                SDL_QueueAudio(g_audio, buf, len);
            }
            free(buf);
        }

        g_sock = INVALID_SOCKET;
        closesocket(s);
        if (!rejected) set_status("Lost connection to %s - reconnecting...", g_cfg.host);
        Sleep(rejected ? 5000 : 2000);
    }
    return 0;
}

static float stick(SDL_GameController *pad, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, float *ox, float *oy) {
    *ox = *oy = 0.0f;
    if (!pad) return 0.0f;
    float x = SDL_GameControllerGetAxis(pad, ax) / 32767.0f;
    float y = SDL_GameControllerGetAxis(pad, ay) / 32767.0f;
    float m = sqrtf(x * x + y * y);
    if (m < PAD_DEADZONE) return 0.0f;
    float s = (m - PAD_DEADZONE) / (1.0f - PAD_DEADZONE);
    if (s > 1.0f) s = 1.0f;
    *ox = x / m * s;
    *oy = y / m * s;
    return s;
}

static void normalize(float *x, float *y) {
    float l = sqrtf(*x * *x + *y * *y);
    if (l > 1.0f) { *x /= l; *y /= l; }
}

int guest_main(const GuestConfig *cfg) {
    g_cfg = *cfg;
    InitializeCriticalSection(&g_lock);
    if (net_startup() != 0) {
        fprintf(stderr, "Winsock failed to start\n");
        return 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window *win = SDL_CreateWindow("Minigore 2 - Player 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!ren) {
        fprintf(stderr, "Could not create window: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof(want));
    want.freq = 44100;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);

    printf("[Co-op] Joining %s:%d as player 2\n", cfg->host, cfg->port);
    fflush(stdout);
    CloseHandle(CreateThread(NULL, 0, connection_thread, NULL, 0, NULL));

    SDL_GameController *pad = NULL;
    SDL_Texture *tex = NULL;
    int tex_w = 0, tex_h = 0;
    SDL_Rect view = { 0, 0, 1280, 720 };
    MsgVideoHeader info;
    memset(&info, 0, sizeof(info));
    float last_aim_x = 0.0f, last_aim_y = -1.0f; /* pad convention: y down, so this is "up" */
    char shown_status[512] = "";
    int running = 1;

    while (running) {
        Uint32 frame_start = SDL_GetTicks();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
            else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && pad) { SDL_GameControllerClose(pad); pad = NULL; }
            else if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_F11 ||
                     (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
                Uint32 full = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(win, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }

        /* Newest frame */
        EnterCriticalSection(&g_lock);
        if (g_frame_new) {
            if (!tex || tex_w != g_frame_w || tex_h != g_frame_h) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, g_frame_w, g_frame_h);
                tex_w = g_frame_w;
                tex_h = g_frame_h;
            }
            if (tex) SDL_UpdateTexture(tex, NULL, g_frame, g_frame_w * 3);
            info = g_frame_info;
            g_frame_new = 0;
        }
        /* Status in the title bar: connection state, or "down" while respawning */
        char status[512];
        if (g_sock != INVALID_SOCKET && g_frame_info.in_level && !g_frame_info.p2_alive) {
            snprintf(status, sizeof(status), "You're down - respawning...");
        } else {
            snprintf(status, sizeof(status), "%s", g_status);
        }
        if (strcmp(shown_status, status) != 0) {
            strcpy(shown_status, status);
            char title[600];
            snprintf(title, sizeof(title), "Minigore 2 - Player 2 - %s", shown_status);
            SDL_SetWindowTitle(win, title);
        }
        LeaveCriticalSection(&g_lock);

        /* Letterboxed view of the stream */
        int ww, wh;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        if (tex_w > 0 && tex_h > 0) {
            float scale = fminf((float)ww / tex_w, (float)wh / tex_h);
            view.w = (int)(tex_w * scale);
            view.h = (int)(tex_h * scale);
            view.x = (ww - view.w) / 2;
            view.y = (wh - view.h) / 2;
        }

        /* Controls -> game pad convention (x right, y down, as on screen) */
        MsgInput in;
        memset(&in, 0, sizeof(in));
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        float mx = 0, my = 0, ax = 0, ay = 0, x, y;
        if (keys[SDL_SCANCODE_W]) my -= 1;
        if (keys[SDL_SCANCODE_S]) my += 1;
        if (keys[SDL_SCANCODE_A]) mx -= 1;
        if (keys[SDL_SCANCODE_D]) mx += 1;
        if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_I]) ay -= 1;
        if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_K]) ay += 1;
        if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_J]) ax -= 1;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_L]) ax += 1;
        if (stick(pad, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &x, &y) > 0) { mx = x; my = y; }
        if (pad) {
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) my -= 1;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) my += 1;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mx -= 1;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) mx += 1;
        }
        if (stick(pad, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &x, &y) > 0) { ax = x; ay = y; }

        /* Mouse: hold the left button to aim from player 2 to the cursor */
        int cx, cy;
        Uint32 mouse = SDL_GetMouseState(&cx, &cy);
        if ((mouse & SDL_BUTTON_LMASK) && info.p2_alive && view.w > 0) {
            float px = view.x + info.p2_x * view.w;
            float py = view.y + info.p2_y * view.h;
            float dx = cx - px, dy = cy - py;
            float l = sqrtf(dx * dx + dy * dy);
            if (l > 8.0f) { ax = dx / l; ay = dy / l; }
            else { ax = last_aim_x; ay = last_aim_y; }
        }

        normalize(&mx, &my);
        normalize(&ax, &ay);
        float al = sqrtf(ax * ax + ay * ay);
        if (al > 0.05f) {
            last_aim_x = ax / al;
            last_aim_y = ay / al;
            in.fire = 1;
        } else if (pad && SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f > TRIGGER_FIRE) {
            /* RT: fire the way you're running, or the last aim */
            float ml = sqrtf(mx * mx + my * my);
            if (ml > 0.3f && SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f < TRIGGER_FIRE) {
                last_aim_x = mx / ml;
                last_aim_y = my / ml;
            }
            ax = last_aim_x;
            ay = last_aim_y;
            in.fire = 1;
        }
        if (in.fire) { /* full deflection: the game uses the direction */
            float l = sqrtf(ax * ax + ay * ay);
            ax /= l;
            ay /= l;
        }
        in.move_x = mx;
        in.move_y = my;
        in.aim_x = ax;
        in.aim_y = ay;
        SOCKET s = g_sock;
        if (s != INVALID_SOCKET) net_send_msg(s, MSG_INPUT, &in, sizeof(in), NULL, 0);

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (tex) SDL_RenderCopy(ren, tex, NULL, &view);

        /* Player 2's own health bar (the host's HUD shows player 1) */
        if (tex && info.in_level && info.p2_alive) {
            int bw = view.w / 4, bh = view.h / 40 + 4;
            SDL_Rect back = { view.x + view.w / 40, view.y + view.h - bh - view.h / 30, bw, bh };
            SDL_Rect fill = { back.x + 2, back.y + 2, (int)((bw - 4) * info.p2_health), bh - 4 };
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 170);
            SDL_RenderFillRect(ren, &back);
            Uint8 red = (Uint8)(255 * (1.0f - info.p2_health)), green = (Uint8)(220 * info.p2_health);
            SDL_SetRenderDrawColor(ren, red, green, 40, 255);
            SDL_RenderFillRect(ren, &fill);
        }
        SDL_RenderPresent(ren);
        Uint32 spent = SDL_GetTicks() - frame_start;
        if (spent < 16) SDL_Delay(16 - spent); /* ~60 Hz even without vsync */
    }

    SOCKET s = g_sock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    if (pad) SDL_GameControllerClose(pad);
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
