#include "android_ndk.h"
#include "determinism.h"
#include "lockstep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>

#ifdef CDECL
#undef CDECL
#endif
#define CDECL __attribute__((cdecl))

/* Forward declarations */
int32_t CDECL wrap_AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent);
int32_t CDECL wrap_AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event);
void CDECL wrap_AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled);
void CDECL wrap_android_app_pre_exec_cmd(struct android_app *app, int8_t cmd);
void CDECL wrap_android_app_post_exec_cmd(struct android_app *app, int8_t cmd);

/* Data structures */
struct AAsset {
    FILE *fp;
    char *buffer;
    size_t length;
    char filename[260];
};

struct AAssetDir {
    HANDLE findHandle;
    WIN32_FIND_DATAA findData;
    int first;
    char currentFile[260];
};

struct AAssetManager {
    int dummy;
};

struct AConfiguration {
    char country[3];
    char language[3];
};

struct ANativeWindow {
    int width;
    int height;
};

struct ALooper {
    int dummy;
};

struct AInputQueue {
    int dummy;
};

#define MAX_TOUCH_POINTERS 3

typedef struct {
    int type; /* 1: key, 2: motion */
    int source;
    int action;
    int flags;
    int metaState;
    int keycode;
    int pointer_count;
    struct {
        int id;
        float x;
        float y;
    } pointers[MAX_TOUCH_POINTERS];
} NativeInputEvent;

#define MAX_EVENTS 256
#define MAX_CMDS   64

static struct android_app g_app;
static ANativeWindow g_window;
static AAssetManager g_asset_manager;
static AConfiguration g_config;
static ALooper g_looper;
static AInputQueue g_input_queue;

static NativeInputEvent g_event_queue[MAX_EVENTS];
static int g_event_head = 0;
static int g_event_tail = 0;

static int8_t g_cmd_queue[MAX_CMDS];
static int g_cmd_head = 0;
static int g_cmd_tail = 0;

static CRITICAL_SECTION g_queue_cs;
static const char *g_data_path = "saves/";

void android_app_set_data_path(const char *path) {
    if (path) g_data_path = path;
}
static HANDLE g_looper_event = NULL;

/* CPU family constants */
#define ANDROID_CPU_FAMILY_X86 2
#define ANDROID_CPU_X86_FEATURE_SSSE3 (1 << 0)
#define ANDROID_CPU_X86_FEATURE_POPCNT (1 << 1)
#define ANDROID_CPU_X86_FEATURE_MOVBE  (1 << 2)

/* Callbacks for poll sources */
static void cmd_process(struct android_app *app, struct android_poll_source *source) {
    int8_t cmd = -1;
    EnterCriticalSection(&g_queue_cs);
    if (g_cmd_head != g_cmd_tail) {
        cmd = g_cmd_queue[g_cmd_tail];
        g_cmd_tail = (g_cmd_tail + 1) % MAX_CMDS;
    }
    LeaveCriticalSection(&g_queue_cs);
    
    printf("[NDK] cmd_process: cmd=%d, onAppCmd=%p\n", cmd, (void *)app->onAppCmd);
    fflush(stdout);
    
    if (cmd >= 0) {
        wrap_android_app_pre_exec_cmd(app, cmd);
        if (app->onAppCmd) {
            app->onAppCmd(app, cmd);
        }
        wrap_android_app_post_exec_cmd(app, cmd);
    }
}

static void input_process(struct android_app *app, struct android_poll_source *source) {
    AInputEvent *event = NULL;
    while (wrap_AInputQueue_getEvent(app->inputQueue, &event) >= 0) {
        if (!wrap_AInputQueue_preDispatchEvent(app->inputQueue, event)) {
            int32_t handled = 0;
            if (app->onInputEvent) {
                handled = app->onInputEvent(app, event);
            }
            wrap_AInputQueue_finishEvent(app->inputQueue, event, handled);
        }
    }
}

/* Dummy JavaVM to safely disable JNI without crash */
static int32_t CDECL dummy_AttachCurrentThread(void *vm, void **p_env, void *thr_args) {
    static volatile LONG calls = 0;
    if (InterlockedIncrement(&calls) <= 3) {
        printf("[JNI] AttachCurrentThread -> JNI_ERR (no Java on Windows)\n");
    }
    if (p_env) *p_env = NULL;
    return -1;
}

static void *g_dummy_jvm_vtable[8] = {
    NULL, /* reserved0 (0x00) */
    NULL, /* reserved1 (0x04) */
    NULL, /* reserved2 (0x08) */
    NULL, /* DestroyJavaVM (0x0c) */
    (void *)dummy_AttachCurrentThread, /* AttachCurrentThread (0x10) */
    NULL, /* DetachCurrentThread (0x14) */
    NULL, /* GetEnv (0x18) */
    NULL  /* AttachCurrentThreadAsDaemon (0x1c) */
};
static void *g_dummy_jvm = g_dummy_jvm_vtable;

void android_ndk_init(int width, int height) {
    InitializeCriticalSection(&g_queue_cs);
    if (!g_looper_event) {
        g_looper_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    
    memset(&g_app, 0, sizeof(g_app));
    memset(&g_window, 0, sizeof(g_window));
    g_window.width = width;
    g_window.height = height;
    
    g_app.window = &g_window;
    g_app.looper = &g_looper;
    g_app.inputQueue = &g_input_queue;
    g_app.config = &g_config;
    
    /* NativeActivity */
    static struct ANativeActivity fake_activity;
    memset(&fake_activity, 0, sizeof(fake_activity));
    fake_activity.vm = &g_dummy_jvm;
    fake_activity.clazz = (void *)1;
    fake_activity.assetManager = &g_asset_manager;
    fake_activity.internalDataPath = g_data_path;
    fake_activity.externalDataPath = g_data_path;
    fake_activity.sdkVersion = 19;
    g_app.activity = &fake_activity;
    
    g_app.cmdPollSource.id = LOOPER_ID_MAIN;
    g_app.cmdPollSource.app = &g_app;
    g_app.cmdPollSource.process = cmd_process;
    
    g_app.inputPollSource.id = LOOPER_ID_INPUT;
    g_app.inputPollSource.app = &g_app;
    g_app.inputPollSource.process = input_process;
}

struct android_app *android_app_get(void) {
    return &g_app;
}

void android_app_push_cmd(int8_t cmd) {
    EnterCriticalSection(&g_queue_cs);
    int next = (g_cmd_head + 1) % MAX_CMDS;
    if (next != g_cmd_tail) {
        g_cmd_queue[g_cmd_head] = cmd;
        g_cmd_head = next;
    }
    LeaveCriticalSection(&g_queue_cs);
    if (g_looper_event) SetEvent(g_looper_event);
}

void android_app_push_touch(int action, int count, const int *ids, const float *xs, const float *ys) {
    if (count < 1 || det_replaying()) return;
    if (count > MAX_TOUCH_POINTERS) count = MAX_TOUCH_POINTERS;
    EnterCriticalSection(&g_queue_cs);

    /* Coalesce consecutive MOVEs of the same fingers: while the game thread
     * is busy (level loads, hitches) held sticks generate hundreds of moves
     * per second. Only the latest positions matter. */
    if (action == AMOTION_EVENT_ACTION_MOVE && g_event_head != g_event_tail) {
        NativeInputEvent *last = &g_event_queue[(g_event_head + MAX_EVENTS - 1) % MAX_EVENTS];
        int same = last->type == AINPUT_EVENT_TYPE_MOTION && last->action == AMOTION_EVENT_ACTION_MOVE &&
                   last->pointer_count == count;
        for (int i = 0; same && i < count; i++) same = last->pointers[i].id == ids[i];
        if (same) {
            for (int i = 0; i < count; i++) {
                last->pointers[i].x = xs[i];
                last->pointers[i].y = ys[i];
            }
            LeaveCriticalSection(&g_queue_cs);
            if (g_looper_event) SetEvent(g_looper_event);
            return;
        }
    }

    int next = (g_event_head + 1) % MAX_EVENTS;
    if (next == g_event_tail) {
        printf("[NDK] Input queue full, dropping motion event (action 0x%x)\n", action);
    } else {
        NativeInputEvent *ev = &g_event_queue[g_event_head];
        memset(ev, 0, sizeof(*ev));
        ev->type = AINPUT_EVENT_TYPE_MOTION;
        ev->source = AINPUT_SOURCE_TOUCHSCREEN;
        ev->action = action;
        ev->pointer_count = count;
        for (int i = 0; i < count; i++) {
            ev->pointers[i].id = ids[i];
            ev->pointers[i].x = xs[i];
            ev->pointers[i].y = ys[i];
        }
        g_event_head = next;
    }
    LeaveCriticalSection(&g_queue_cs);
    if (g_looper_event) SetEvent(g_looper_event);
}

void android_app_push_key_event(int action, int keycode) {
    if (det_replaying()) return;
    EnterCriticalSection(&g_queue_cs);
    int next = (g_event_head + 1) % MAX_EVENTS;
    if (next != g_event_tail) {
        NativeInputEvent *ev = &g_event_queue[g_event_head];
        ev->type = AINPUT_EVENT_TYPE_KEY;
        ev->source = AINPUT_SOURCE_KEYBOARD;
        ev->action = action;
        ev->flags = 0;
        ev->metaState = 0;
        ev->keycode = keycode;
        g_event_head = next;
    }
    LeaveCriticalSection(&g_queue_cs);
    if (g_looper_event) SetEvent(g_looper_event);
}

size_t android_app_event_size(void) {
    return sizeof(NativeInputEvent);
}

int android_app_drain_events(void *out, int max) {
    NativeInputEvent *dst = (NativeInputEvent *)out;
    int n = 0;
    EnterCriticalSection(&g_queue_cs);
    while (n < max && g_event_head != g_event_tail) {
        dst[n++] = g_event_queue[g_event_tail];
        g_event_tail = (g_event_tail + 1) % MAX_EVENTS;
    }
    LeaveCriticalSection(&g_queue_cs);
    return n;
}

void android_app_set_window_size(int width, int height) {
    g_window.width = width;
    g_window.height = height;
}

int android_app_input_queue_depth(void) {
    EnterCriticalSection(&g_queue_cs);
    int depth = (g_event_head - g_event_tail + MAX_EVENTS) % MAX_EVENTS;
    LeaveCriticalSection(&g_queue_cs);
    return depth;
}

int android_app_is_destroy_requested(void) {
    return g_app.destroyRequested;
}

/* NDK APIs (__cdecl) */
AAsset * CDECL wrap_AAssetManager_open(AAssetManager *mgr, const char *filename, int mode) {
    if (!filename) return NULL;
    char path[512];
    snprintf(path, sizeof(path), "assets/%s", filename);
    
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        /* Try without assets/ prefix */
        fp = fopen(filename, "rb");
        if (!fp) {
            printf("[Asset] Not found: %s\n", filename);
            return NULL;
        }
    }
    
    /* No per-call logging here: assets (including streamed music) are read
     * constantly, and every console line used to stall the game thread. */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    AAsset *asset = (AAsset *)calloc(1, sizeof(AAsset));
    asset->fp = fp;
    asset->length = (size_t)sz;
    strncpy(asset->filename, path, sizeof(asset->filename) - 1);
    return asset;
}

int CDECL wrap_AAsset_read(AAsset *asset, void *buf, size_t count) {
    if (!asset || !asset->fp) return -1;
    return (int)fread(buf, 1, count, asset->fp);
}

off_t CDECL wrap_AAsset_seek(AAsset *asset, off_t offset, int whence) {
    if (!asset || !asset->fp) return -1;
    fseek(asset->fp, (long)offset, whence);
    return (off_t)ftell(asset->fp);
}

off_t CDECL wrap_AAsset_getLength(AAsset *asset) {
    if (!asset) return 0;
    return (off_t)asset->length;
}

const void * CDECL wrap_AAsset_getBuffer(AAsset *asset) {
    if (!asset || !asset->fp) return NULL;
    if (!asset->buffer) {
        asset->buffer = (char *)malloc(asset->length + 1);
        long cur = ftell(asset->fp);
        fseek(asset->fp, 0, SEEK_SET);
        size_t rd = fread(asset->buffer, 1, asset->length, asset->fp);
        asset->buffer[asset->length] = '\0';
        fseek(asset->fp, cur, SEEK_SET);
        if (rd != asset->length) printf("[Asset] Short read of %s: %zu of %zu bytes\n", asset->filename, rd, asset->length);
    }
    return asset->buffer;
}

void CDECL wrap_AAsset_close(AAsset *asset) {
    if (!asset) return;
    if (asset->fp) fclose(asset->fp);
    if (asset->buffer) free(asset->buffer);
    free(asset);
}

int CDECL wrap_AAsset_openFileDescriptor(AAsset *asset, off_t *outStart, off_t *outLength) {
    if (!asset) return -1;
    if (outStart) *outStart = 0;
    if (outLength) *outLength = (off_t)asset->length;
    int fd = _open(asset->filename, _O_RDONLY | _O_BINARY);
    return fd;
}

AAssetDir * CDECL wrap_AAssetManager_openDir(AAssetManager *mgr, const char *dirName) {
    char pattern[512];
    if (dirName && strlen(dirName) > 0) {
        snprintf(pattern, sizeof(pattern), "assets/%s/*", dirName);
    } else {
        snprintf(pattern, sizeof(pattern), "assets/*");
    }
    
    AAssetDir *dir = (AAssetDir *)calloc(1, sizeof(AAssetDir));
    dir->findHandle = FindFirstFileA(pattern, &dir->findData);
    dir->first = 1;
    return dir;
}

const char * CDECL wrap_AAssetDir_getNextFileName(AAssetDir *assetDir) {
    if (!assetDir || assetDir->findHandle == INVALID_HANDLE_VALUE) return NULL;
    
    while (1) {
        if (!assetDir->first) {
            if (!FindNextFileA(assetDir->findHandle, &assetDir->findData)) {
                return NULL;
            }
        }
        assetDir->first = 0;
        
        if (strcmp(assetDir->findData.cFileName, ".") != 0 &&
            strcmp(assetDir->findData.cFileName, "..") != 0 &&
            !(assetDir->findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            strncpy(assetDir->currentFile, assetDir->findData.cFileName, sizeof(assetDir->currentFile) - 1);
            return assetDir->currentFile;
        }
    }
}

void CDECL wrap_AAssetDir_close(AAssetDir *assetDir) {
    if (!assetDir) return;
    if (assetDir->findHandle != INVALID_HANDLE_VALUE) {
        FindClose(assetDir->findHandle);
    }
    free(assetDir);
}

AConfiguration * CDECL wrap_AConfiguration_new(void) {
    return &g_config;
}

void CDECL wrap_AConfiguration_delete(AConfiguration *config) {}

void CDECL wrap_AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *am) {}

void CDECL wrap_AConfiguration_getCountry(AConfiguration *config, char *out) {
    if (out) {
        out[0] = 'U';
        out[1] = 'S';
        out[2] = '\0';
    }
}

void CDECL wrap_AConfiguration_getLanguage(AConfiguration *config, char *out) {
    if (out) {
        out[0] = 'e';
        out[1] = 'n';
        out[2] = '\0';
    }
}

int32_t CDECL wrap_ANativeWindow_getWidth(ANativeWindow *window) {
    return window ? window->width : 1280;
}

int32_t CDECL wrap_ANativeWindow_getHeight(ANativeWindow *window) {
    return window ? window->height : 720;
}

int32_t CDECL wrap_ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format) {
    return 0;
}

ALooper * CDECL wrap_ALooper_prepare(int opts) {
    return &g_looper;
}

int CDECL wrap_ALooper_addFd(ALooper *looper, int fd, int ident, int events, void *callback, void *data) {
    return 1;
}

int CDECL wrap_ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    while (1) {
        EnterCriticalSection(&g_queue_cs);
        if (g_cmd_head != g_cmd_tail) {
            if (outData) *outData = &g_app.cmdPollSource;
            LeaveCriticalSection(&g_queue_cs);
            return LOOPER_ID_MAIN;
        }
        /* Own-world co-op: the game only sees player 1's scheduled events
         * (this PC's own input waits in the queue for lockstep to send) */
        int pending = lockstep_active() ? lockstep_event_pending()
                                        : (g_event_head != g_event_tail || det_replay_pending());
        if (pending) {
            if (outData) *outData = &g_app.inputPollSource;
            LeaveCriticalSection(&g_queue_cs);
            return LOOPER_ID_INPUT;
        }
        LeaveCriticalSection(&g_queue_cs);
        
        if (timeoutMillis == 0) {
            det_idle(); /* determinism test: let virtual time pass */
            return -3; /* ALOOPER_POLL_TIMEOUT */
        }
        
        DWORD waitMs = (timeoutMillis < 0) ? 16 : (DWORD)timeoutMillis;
        DWORD res = WaitForSingleObject(g_looper_event, waitMs);
        if (res == WAIT_TIMEOUT || timeoutMillis > 0 || timeoutMillis < 0) {
            return -3;
        }
    }
}

void CDECL wrap_ALooper_wake(ALooper *looper) {
    if (g_looper_event) SetEvent(g_looper_event);
}

void CDECL wrap_AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident, void *callback, void *data) {}
void CDECL wrap_AInputQueue_detachLooper(AInputQueue *queue) {}

int32_t CDECL wrap_AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent) {
    if (det_replaying() || lockstep_active()) {
        /* Determinism replay: the recorded events, frame by frame; own-world
         * co-op: player 1's events for this frame */
        NativeInputEvent tmp;
        int got = lockstep_active() ? lockstep_take_event(&tmp, sizeof(tmp)) : det_replay_event(&tmp, sizeof(tmp));
        if (!got) return -1;
        NativeInputEvent *ev = (NativeInputEvent *)malloc(sizeof(NativeInputEvent));
        *ev = tmp;
        *outEvent = (AInputEvent *)ev;
        return 0;
    }
    EnterCriticalSection(&g_queue_cs);
    if (g_event_head != g_event_tail) {
        NativeInputEvent *ev = (NativeInputEvent *)malloc(sizeof(NativeInputEvent));
        *ev = g_event_queue[g_event_tail];
        g_event_tail = (g_event_tail + 1) % MAX_EVENTS;
        *outEvent = (AInputEvent *)ev;
        LeaveCriticalSection(&g_queue_cs);
        det_record_event(ev, sizeof(*ev));
        return 0;
    }
    LeaveCriticalSection(&g_queue_cs);
    return -1;
}

int32_t CDECL wrap_AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event) {
    return 0;
}

void CDECL wrap_AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled) {
    if (event) free(event);
}

int32_t CDECL wrap_AInputEvent_getType(const AInputEvent *event) {
    return event ? ((NativeInputEvent *)event)->type : 0;
}

int32_t CDECL wrap_AInputEvent_getDeviceId(const AInputEvent *event) {
    return 0;
}

int32_t CDECL wrap_AInputEvent_getSource(const AInputEvent *event) {
    return event ? ((NativeInputEvent *)event)->source : AINPUT_SOURCE_TOUCHSCREEN;
}

int32_t CDECL wrap_AMotionEvent_getAction(const AInputEvent *event) {
    return event ? ((NativeInputEvent *)event)->action : 0;
}

int32_t CDECL wrap_AMotionEvent_getFlags(const AInputEvent *event) {
    return 0;
}

int32_t CDECL wrap_AMotionEvent_getMetaState(const AInputEvent *event) {
    return 0;
}

size_t CDECL wrap_AMotionEvent_getPointerCount(const AInputEvent *event) {
    return event ? ((NativeInputEvent *)event)->pointer_count : 1;
}

int32_t CDECL wrap_AMotionEvent_getPointerId(const AInputEvent *event, size_t pointer_index) {
    if (!event) return (int32_t)pointer_index;
    NativeInputEvent *ev = (NativeInputEvent *)event;
    if (pointer_index < (size_t)ev->pointer_count && pointer_index < MAX_TOUCH_POINTERS) return ev->pointers[pointer_index].id;
    return (int32_t)pointer_index;
}

float CDECL wrap_AMotionEvent_getX(const AInputEvent *event, size_t pointer_index) {
    if (!event) return 0.0f;
    NativeInputEvent *ev = (NativeInputEvent *)event;
    if (pointer_index < (size_t)ev->pointer_count && pointer_index < MAX_TOUCH_POINTERS) return ev->pointers[pointer_index].x;
    return 0.0f;
}

float CDECL wrap_AMotionEvent_getY(const AInputEvent *event, size_t pointer_index) {
    if (!event) return 0.0f;
    NativeInputEvent *ev = (NativeInputEvent *)event;
    if (pointer_index < (size_t)ev->pointer_count && pointer_index < MAX_TOUCH_POINTERS) return ev->pointers[pointer_index].y;
    return 0.0f;
}

size_t CDECL wrap_AMotionEvent_getHistorySize(const AInputEvent *event) {
    return 0;
}

float CDECL wrap_AMotionEvent_getHistoricalX(const AInputEvent *event, size_t pointer_index, size_t history_pos) {
    return wrap_AMotionEvent_getX(event, pointer_index);
}

float CDECL wrap_AMotionEvent_getHistoricalY(const AInputEvent *event, size_t pointer_index, size_t history_pos) {
    return wrap_AMotionEvent_getY(event, pointer_index);
}

int32_t CDECL wrap_AKeyEvent_getAction(const AInputEvent *event) {
    return event ? ((NativeInputEvent *)event)->action : 0;
}

int32_t CDECL wrap_AKeyEvent_getKeyCode(const AInputEvent *event) {
    return event ? ((NativeInputEvent *)event)->keycode : 0;
}

int32_t CDECL wrap_AKeyEvent_getFlags(const AInputEvent *event) {
    return 0;
}

int32_t CDECL wrap_AKeyEvent_getMetaState(const AInputEvent *event) {
    return 0;
}

void CDECL wrap_ANativeActivity_finish(ANativeActivity *activity) {
    printf("[NDK] wrap_ANativeActivity_finish called!\n");
    fflush(stdout);
    g_app.destroyRequested = 1;
}

int CDECL wrap_android_getCpuFamily(void) {
    return ANDROID_CPU_FAMILY_X86;
}

uint64_t CDECL wrap_android_getCpuFeatures(void) {
    return ANDROID_CPU_X86_FEATURE_SSSE3 | ANDROID_CPU_X86_FEATURE_POPCNT | ANDROID_CPU_X86_FEATURE_MOVBE;
}

int CDECL wrap_android_getCpuCount(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 4;
}

void CDECL wrap_android_setCpu(int count, uint64_t features) {}

void CDECL wrap_app_dummy(void) {}

int8_t CDECL wrap_android_app_read_cmd(struct android_app *app) {
    int8_t cmd = -1;
    EnterCriticalSection(&g_queue_cs);
    if (g_cmd_head != g_cmd_tail) {
        cmd = g_cmd_queue[g_cmd_tail];
        g_cmd_tail = (g_cmd_tail + 1) % MAX_CMDS;
    }
    LeaveCriticalSection(&g_queue_cs);
    return cmd;
}

void CDECL wrap_android_app_pre_exec_cmd(struct android_app *app, int8_t cmd) {
    if (cmd == APP_CMD_INIT_WINDOW) {
        app->activityState = 1;
    }
}

void CDECL wrap_android_app_post_exec_cmd(struct android_app *app, int8_t cmd) {
    if (cmd == APP_CMD_TERM_WINDOW) {
        app->activityState = 0;
    }
}

int CDECL wrap___android_log_print(int prio, const char *tag, const char *fmt, ...) {
    static char s_last_msg[256] = {0};
    static int s_repeat_count = 0;
    
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    
    if (strcmp(msg, s_last_msg) == 0) {
        s_repeat_count++;
        if (s_repeat_count == 10) {
            printf("[%s] (previous message repeated 10 times, suppressing further duplicates...)\n", tag ? tag : "LOG");
            fflush(stdout);
        }
        return 0;
    }
    
    if (s_repeat_count >= 10) {
        printf("[%s] (previous message repeated %d times)\n", tag ? tag : "LOG", s_repeat_count);
    }
    s_repeat_count = 0;
    strncpy(s_last_msg, msg, sizeof(s_last_msg) - 1);
    s_last_msg[sizeof(s_last_msg) - 1] = '\0';
    
    printf("[%s] %s\n", tag ? tag : "LOG", msg);
    fflush(stdout);
    return 0;
}
