#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "elf_loader.h"
#include "shims.h"
#include "gl_wrap.h"
#include "egl_wrap.h"
#include "android_ndk.h"
#include "opensles.h"

typedef void (*android_main_fn)(struct android_app *state);

static SDL_Window *g_window = NULL;
static SDL_GLContext g_gl_ctx = NULL;
static HANDLE g_game_thread = NULL;
static bool g_running = true;
static int g_win_w = 1280;
static int g_win_h = 720;

/* Virtual touch joystick state */
static bool g_wasd_active = false;
static float g_wasd_x = 0.0f;
static float g_wasd_y = 0.0f;

static bool g_aim_active = false;
static float g_aim_x = 0.0f;
static float g_aim_y = 0.0f;

static bool g_mouse_active = false;
static float g_mouse_x = 0.0f;
static float g_mouse_y = 0.0f;

static SDL_GameController *g_gamepad = NULL;

static void update_touch_events(void) {
    bool right_active = g_mouse_active || g_aim_active;
    float right_x = g_mouse_active ? g_mouse_x : g_aim_x;
    float right_y = g_mouse_active ? g_mouse_y : g_aim_y;
    
    if (g_wasd_active && right_active) {
        /* Dual touch: pointer 0 is movement, pointer 1 is aim/action */
        android_app_push_motion_event(AMOTION_EVENT_ACTION_MOVE, 2,
                                      g_wasd_x, g_wasd_y,
                                      right_x, right_y);
    } else if (g_wasd_active) {
        /* Single touch: movement */
        android_app_push_motion_event(AMOTION_EVENT_ACTION_MOVE, 1,
                                      g_wasd_x, g_wasd_y,
                                      0.0f, 0.0f);
    } else if (right_active) {
        /* Single touch: mouse / aim */
        android_app_push_motion_event(AMOTION_EVENT_ACTION_MOVE, 1,
                                      right_x, right_y,
                                      0.0f, 0.0f);
    }
}

static void update_wasd_stick(void) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    float dx = 0.0f;
    float dy = 0.0f;
    
    if (keys[SDL_SCANCODE_W]) dy -= 1.0f;
    if (keys[SDL_SCANCODE_S]) dy += 1.0f;
    if (keys[SDL_SCANCODE_A]) dx -= 1.0f;
    if (keys[SDL_SCANCODE_D]) dx += 1.0f;
    
    /* Also check gamepad left stick */
    if (g_gamepad) {
        Sint16 ax = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(ax) > 8000 || abs(ay) > 8000) {
            dx = (float)ax / 32767.0f;
            dy = (float)ay / 32767.0f;
        }
    }
    
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.001f) {
        if (len > 1.0f) {
            dx /= len;
            dy /= len;
        }
        
        float base_x = (float)g_win_w * 0.18f;
        float base_y = (float)g_win_h * 0.72f;
        float radius = (float)g_win_w * 0.09f;
        
        g_wasd_x = base_x + dx * radius;
        g_wasd_y = base_y + dy * radius;
        
        if (!g_wasd_active) {
            g_wasd_active = true;
            bool right_active = g_mouse_active || g_aim_active;
            float right_x = g_mouse_active ? g_mouse_x : g_aim_x;
            float right_y = g_mouse_active ? g_mouse_y : g_aim_y;
            if (right_active) {
                android_app_push_motion_event(AMOTION_EVENT_ACTION_POINTER_DOWN | (0 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                                              2, g_wasd_x, g_wasd_y, right_x, right_y);
            } else {
                android_app_push_motion_event(AMOTION_EVENT_ACTION_DOWN, 1,
                                              g_wasd_x, g_wasd_y, 0.0f, 0.0f);
            }
        } else {
            update_touch_events();
        }
    } else {
        if (g_wasd_active) {
            g_wasd_active = false;
            bool right_active = g_mouse_active || g_aim_active;
            float right_x = g_mouse_active ? g_mouse_x : g_aim_x;
            float right_y = g_mouse_active ? g_mouse_y : g_aim_y;
            if (right_active) {
                android_app_push_motion_event(AMOTION_EVENT_ACTION_POINTER_UP | (0 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                                              2, g_wasd_x, g_wasd_y, right_x, right_y);
            } else {
                android_app_push_motion_event(AMOTION_EVENT_ACTION_UP, 1,
                                              g_wasd_x, g_wasd_y, 0.0f, 0.0f);
            }
        }
    }
}

static void update_aim_stick(void) {
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    float dx = 0.0f;
    float dy = 0.0f;
    
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_I]) dy -= 1.0f;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_K]) dy += 1.0f;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_J]) dx -= 1.0f;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_L]) dx += 1.0f;
    
    /* Also check gamepad right stick */
    if (g_gamepad) {
        Sint16 ax = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_RIGHTX);
        Sint16 ay = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_RIGHTY);
        if (abs(ax) > 8000 || abs(ay) > 8000) {
            dx = (float)ax / 32767.0f;
            dy = (float)ay / 32767.0f;
        }
    }
    
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 0.001f) {
        if (len > 1.0f) {
            dx /= len;
            dy /= len;
        }
        
        float base_x = (float)g_win_w * 0.82f;
        float base_y = (float)g_win_h * 0.72f;
        float radius = (float)g_win_w * 0.09f;
        
        g_aim_x = base_x + dx * radius;
        g_aim_y = base_y + dy * radius;
        
        if (!g_aim_active) {
            g_aim_active = true;
            if (g_wasd_active) {
                android_app_push_motion_event(AMOTION_EVENT_ACTION_POINTER_DOWN | (1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                                              2, g_wasd_x, g_wasd_y, g_aim_x, g_aim_y);
            } else {
                android_app_push_motion_event(AMOTION_EVENT_ACTION_DOWN, 1,
                                              g_aim_x, g_aim_y, 0.0f, 0.0f);
            }
        } else {
            update_touch_events();
        }
    } else {
        if (g_aim_active) {
            g_aim_active = false;
            if (g_wasd_active) {
                android_app_push_motion_event(AMOTION_EVENT_ACTION_POINTER_UP | (1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                                              2, g_wasd_x, g_wasd_y, g_aim_x, g_aim_y);
            } else {
                android_app_push_motion_event(AMOTION_EVENT_ACTION_UP, 1,
                                              g_aim_x, g_aim_y, 0.0f, 0.0f);
            }
        }
    }
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)fault_addr, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_RESERVE) {
                /* Reserved stack page accessed past guard page - commit on demand */
                void *page = (void *)(fault_addr & ~0xFFF);
                VirtualAlloc(page, 64 * 1024, MEM_COMMIT, PAGE_READWRITE);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    printf("\n================ CRASH / EXCEPTION CAUGHT ================\n");
    printf("Exception Code: 0x%08lX\n", ep->ExceptionRecord->ExceptionCode);
    printf("Exception Address: %p\n", ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        printf("Access Violation: %s at %p\n",
               ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "Read" :
               ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "Write" : "Execute",
               (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    PCONTEXT ctx = ep->ContextRecord;
    printf("EAX: 0x%08lX  EBX: 0x%08lX  ECX: 0x%08lX  EDX: 0x%08lX\n",
           ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    printf("ESI: 0x%08lX  EDI: 0x%08lX  EBP: 0x%08lX  ESP: 0x%08lX\n",
           ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    printf("EIP: 0x%08lX\n", ctx->Eip);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI game_thread_proc(LPVOID data) {
    android_main_fn p_main = (android_main_fn)data;
    printf("[Thread] Entering android_main...\n");
    fflush(stdout);

    /* Commit entire thread stack allocation upfront */
    MEMORY_BASIC_INFORMATION mbi;
    char *p = (char *)&mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi))) {
        if (mbi.AllocationBase) {
            SIZE_T size_to_commit = (SIZE_T)(p - (char *)mbi.AllocationBase);
            VirtualAlloc(mbi.AllocationBase, size_to_commit, MEM_COMMIT, PAGE_READWRITE);
        }
    }
    
    p_main(android_app_get());
    
    printf("[Thread] android_main returned.\n");
    fflush(stdout);
    g_running = false;
    return 0;
}

int main(int argc, char *argv[]) {
    AddVectoredExceptionHandler(1, crash_handler);
    
    printf("====================================================\n");
    printf("  Minigore 2: Zombies - Native Windows Recompilation\n");
    printf("  Direct Android x86 ELF Loader under WOW64\n");
    printf("====================================================\n\n");
    fflush(stdout);
    
    /* Ensure saves folder exists */
    CreateDirectoryA("saves", NULL);
    
    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        return 1;
    }
    
    /* Configure OpenGL 2.1 Compatibility Context */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    
    g_window = SDL_CreateWindow(
        "Minigore 2: Zombies",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_win_w, g_win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    
    if (!g_window) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    
    g_gl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_gl_ctx) {
        fprintf(stderr, "Failed to create OpenGL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }
    
    SDL_GL_SetSwapInterval(1); /* VSync */
    
    printf("[Host] OpenGL Context: %s\n", (const char *)glGetString(GL_VERSION));
    printf("[Host] OpenGL Renderer: %s\n", (const char *)glGetString(GL_RENDERER));
    fflush(stdout);
    
    /* Initialize host wrapper subsystems */
    shims_init();
    gl_wrap_init();
    egl_wrap_init(g_window, g_gl_ctx);
    opensles_init();
    android_ndk_init(g_win_w, g_win_h);
    
    /* Release OpenGL context from main thread so game thread can bind it */
    SDL_GL_MakeCurrent(g_window, NULL);
    
    /* Load libminigore2.so */
    elf_module_t mod;
    size_t sym_count = 0;
    const host_symbol_t *syms = shims_get_table(&sym_count);
    
    printf("[Host] Loading lib/x86/libminigore2.so...\n");
    fflush(stdout);
    
    if (elf_load("lib/x86/libminigore2.so", &mod) != 0) {
        fprintf(stderr, "[Host] Failed to load libminigore2.so\n");
        return 1;
    }
    
    printf("[Host] Relocating ELF symbols...\n");
    fflush(stdout);
    
    if (elf_relocate(&mod, syms, sym_count) != 0) {
        fprintf(stderr, "[Host] Failed to relocate ELF module\n");
        return 1;
    }
    
    printf("[Host] Executing ELF constructors...\n");
    fflush(stdout);
    elf_init(&mod);
    
    /* Find android_main entry point */
    android_main_fn p_android_main = (android_main_fn)elf_sym(&mod, "android_main");
    if (!p_android_main) {
        fprintf(stderr, "[Host] Symbol 'android_main' not found in module!\n");
        return 1;
    }
    
    /* Push initial Android lifecycle commands so they are waiting when android_main begins polling */
    printf("[Host] Queueing Android lifecycle commands (START, INIT_WINDOW, RESUME, GAINED_FOCUS)...\n");
    fflush(stdout);
    android_app_push_cmd(APP_CMD_START);
    android_app_push_cmd(APP_CMD_INIT_WINDOW);
    android_app_push_cmd(APP_CMD_RESUME);
    android_app_push_cmd(APP_CMD_GAINED_FOCUS);
    
    printf("[Host] Starting game thread with 16MB committed stack...\n");
    fflush(stdout);
    g_game_thread = CreateThread(NULL, 16 * 1024 * 1024, game_thread_proc, (void *)p_android_main, 0, NULL);
    if (!g_game_thread) {
        fprintf(stderr, "[Host] Failed to create game thread: %lu\n", GetLastError());
        return 1;
    }
    
    /* Main Event Loop */
    SDL_Event ev;
    while (g_running) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    printf("[Host] Quit event received.\n");
                    g_running = false;
                    break;
                    
                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                        ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        g_win_w = ev.window.data1;
                        g_win_h = ev.window.data2;
                        android_app_push_cmd(APP_CMD_WINDOW_RESIZED);
                    } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                        android_app_push_cmd(APP_CMD_GAINED_FOCUS);
                    } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        android_app_push_cmd(APP_CMD_LOST_FOCUS);
                    }
                    break;
                    
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        g_mouse_active = true;
                        g_mouse_x = (float)ev.button.x;
                        g_mouse_y = (float)ev.button.y;
                        
                        if (g_wasd_active) {
                            android_app_push_motion_event(AMOTION_EVENT_ACTION_POINTER_DOWN | (1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                                                          2, g_wasd_x, g_wasd_y, g_mouse_x, g_mouse_y);
                        } else {
                            android_app_push_motion_event(AMOTION_EVENT_ACTION_DOWN, 1,
                                                          g_mouse_x, g_mouse_y, 0.0f, 0.0f);
                        }
                    }
                    break;
                    
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        g_mouse_active = false;
                        g_mouse_x = (float)ev.button.x;
                        g_mouse_y = (float)ev.button.y;
                        
                        if (g_wasd_active) {
                            android_app_push_motion_event(AMOTION_EVENT_ACTION_POINTER_UP | (1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                                                          2, g_wasd_x, g_wasd_y, g_mouse_x, g_mouse_y);
                        } else {
                            android_app_push_motion_event(AMOTION_EVENT_ACTION_UP, 1,
                                                          g_mouse_x, g_mouse_y, 0.0f, 0.0f);
                        }
                    }
                    break;
                    
                case SDL_MOUSEMOTION:
                    if (g_mouse_active) {
                        g_mouse_x = (float)ev.motion.x;
                        g_mouse_y = (float)ev.motion.y;
                        update_touch_events();
                    }
                    break;
                    
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    int action = (ev.type == SDL_KEYDOWN) ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
                    
                    /* Update WASD virtual movement */
                    if (ev.key.keysym.scancode == SDL_SCANCODE_W ||
                        ev.key.keysym.scancode == SDL_SCANCODE_A ||
                        ev.key.keysym.scancode == SDL_SCANCODE_S ||
                        ev.key.keysym.scancode == SDL_SCANCODE_D) {
                        update_wasd_stick();
                    }
                    
                    /* Update Aim virtual stick (Arrow keys / IJKL) */
                    if (ev.key.keysym.scancode == SDL_SCANCODE_UP ||
                        ev.key.keysym.scancode == SDL_SCANCODE_DOWN ||
                        ev.key.keysym.scancode == SDL_SCANCODE_LEFT ||
                        ev.key.keysym.scancode == SDL_SCANCODE_RIGHT ||
                        ev.key.keysym.scancode == SDL_SCANCODE_I ||
                        ev.key.keysym.scancode == SDL_SCANCODE_K ||
                        ev.key.keysym.scancode == SDL_SCANCODE_J ||
                        ev.key.keysym.scancode == SDL_SCANCODE_L) {
                        update_aim_stick();
                    }
                    
                    /* Back button / ESC */
                    if (ev.key.keysym.sym == SDLK_ESCAPE) {
                        android_app_push_key_event(action, AKEYCODE_BACK);
                    } else if (ev.key.keysym.sym == SDLK_SPACE || ev.key.keysym.sym == SDLK_RETURN) {
                        android_app_push_key_event(action, AKEYCODE_BUTTON_A);
                    }
                    break;
                }
                
                case SDL_CONTROLLERDEVICEADDED:
                    if (!g_gamepad) {
                        g_gamepad = SDL_GameControllerOpen(ev.cdevice.which);
                        if (g_gamepad) {
                            printf("[Host] Gamepad connected: %s\n", SDL_GameControllerName(g_gamepad));
                            fflush(stdout);
                        }
                    }
                    break;
                    
                case SDL_CONTROLLERDEVICEREMOVED:
                    if (g_gamepad) {
                        SDL_GameControllerClose(g_gamepad);
                        g_gamepad = NULL;
                        printf("[Host] Gamepad disconnected.\n");
                        fflush(stdout);
                    }
                    break;
                    
                case SDL_CONTROLLERAXISMOTION:
                    update_wasd_stick();
                    update_aim_stick();
                    break;
                    
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    int action = (ev.type == SDL_CONTROLLERBUTTONDOWN) ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
                    if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                        android_app_push_key_event(action, AKEYCODE_BUTTON_A);
                    } else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B || ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                        android_app_push_key_event(action, AKEYCODE_BACK);
                    }
                    break;
                }
                
                default:
                    break;
            }
        }
        
        if (android_app_is_destroy_requested()) {
            printf("[Host] Game requested destroy.\n");
            g_running = false;
        }
        
        SDL_Delay(10);
    }
    
    printf("[Host] Shutting down application...\n");
    android_app_push_cmd(APP_CMD_LOST_FOCUS);
    android_app_push_cmd(APP_CMD_PAUSE);
    android_app_push_cmd(APP_CMD_TERM_WINDOW);
    android_app_push_cmd(APP_CMD_DESTROY);
    
    if (g_game_thread) {
        WaitForSingleObject(g_game_thread, 2000);
        CloseHandle(g_game_thread);
    }
    
    opensles_shutdown();
    
    if (g_gl_ctx) {
        SDL_GL_DeleteContext(g_gl_ctx);
    }
    if (g_gamepad) {
        SDL_GameControllerClose(g_gamepad);
        g_gamepad = NULL;
    }
    
    if (g_window) {
        SDL_DestroyWindow(g_window);
    }
    
    SDL_Quit();
    
    printf("[Host] Exited cleanly.\n");
    return 0;
}
