#ifndef ANDROID_NDK_H
#define ANDROID_NDK_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
typedef struct AAssetDir AAssetDir;
typedef struct AConfiguration AConfiguration;
typedef struct ANativeWindow ANativeWindow;
typedef struct ALooper ALooper;
typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;

struct ANativeActivityCallbacks;

typedef struct ANativeActivity {
    struct ANativeActivityCallbacks* callbacks;
    void* vm;
    void* env;
    void* clazz;
    const char* internalDataPath;
    const char* externalDataPath;
    int32_t sdkVersion;
    void* instance;
    AAssetManager* assetManager;
    const char* obbPath;
} ANativeActivity;

/* Lifecycle commands */
enum {
    APP_CMD_INPUT_CHANGED = 0,
    APP_CMD_INIT_WINDOW = 1,
    APP_CMD_TERM_WINDOW = 2,
    APP_CMD_WINDOW_RESIZED = 3,
    APP_CMD_WINDOW_REDRAW_NEEDED = 4,
    APP_CMD_CONTENT_RECT_CHANGED = 5,
    APP_CMD_GAINED_FOCUS = 6,
    APP_CMD_LOST_FOCUS = 7,
    APP_CMD_CONFIG_CHANGED = 8,
    APP_CMD_LOW_MEMORY = 9,
    APP_CMD_START = 10,
    APP_CMD_RESUME = 11,
    APP_CMD_SAVE_STATE = 12,
    APP_CMD_PAUSE = 13,
    APP_CMD_STOP = 14,
    APP_CMD_DESTROY = 15,
};

/* Input event constants */
enum {
    AINPUT_EVENT_TYPE_KEY = 1,
    AINPUT_EVENT_TYPE_MOTION = 2
};

enum {
    AINPUT_SOURCE_KEYBOARD = 0x00000101,
    AINPUT_SOURCE_TOUCHSCREEN = 0x00001002,
    AINPUT_SOURCE_JOYSTICK = 0x01000010
};

enum {
    AKEY_EVENT_ACTION_DOWN = 0,
    AKEY_EVENT_ACTION_UP = 1
};

enum {
    AMOTION_EVENT_ACTION_MASK = 0xff,
    AMOTION_EVENT_ACTION_POINTER_INDEX_MASK = 0xff00,
    AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT = 8,
    AMOTION_EVENT_ACTION_DOWN = 0,
    AMOTION_EVENT_ACTION_UP = 1,
    AMOTION_EVENT_ACTION_MOVE = 2,
    AMOTION_EVENT_ACTION_CANCEL = 3,
    AMOTION_EVENT_ACTION_POINTER_DOWN = 5,
    AMOTION_EVENT_ACTION_POINTER_UP = 6
};

/* Keycodes */
enum {
    AKEYCODE_UNKNOWN = 0,
    AKEYCODE_BACK = 4,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_DPAD_CENTER = 23,
    AKEYCODE_BUTTON_A = 96,
    AKEYCODE_BUTTON_B = 97,
    AKEYCODE_BUTTON_X = 99,
    AKEYCODE_BUTTON_Y = 100,
    AKEYCODE_BUTTON_L1 = 102,
    AKEYCODE_BUTTON_R1 = 103,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109
};

/* Looper IDs */
enum {
    LOOPER_ID_MAIN = 1,
    LOOPER_ID_INPUT = 2,
    LOOPER_ID_USER = 3
};

struct android_poll_source {
    int32_t id;
    struct android_app *app;
    void (*process)(struct android_app *app, struct android_poll_source *source);
};

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} ARect;

struct android_app {
    void *userData;
    void (*onAppCmd)(struct android_app *app, int32_t cmd);
    int32_t (*onInputEvent)(struct android_app *app, AInputEvent *event);
    ANativeActivity *activity;
    AConfiguration *config;
    void *savedState;
    size_t savedStateSize;
    ALooper *looper;
    AInputQueue *inputQueue;
    ANativeWindow *window;
    ARect contentRect;
    int activityState;
    int destroyRequested;
    void *mutex;
    void *cond;
    int msgread;
    int msgwrite;
    uintptr_t thread;
    struct android_poll_source cmdPollSource;
    struct android_poll_source inputPollSource;
    int running;
    int stateSaved;
    int destroyed;
    void *pendingInputQueue;
    void *pendingWindow;
    ARect pendingContentRect;
};

/* Host control APIs */
void android_ndk_init(int width, int height);
struct android_app *android_app_get(void);
void android_app_push_cmd(int8_t cmd);
void android_app_push_motion_event(int action, int pointer_count, float x0, float y0, float x1, float y1);
void android_app_push_key_event(int action, int keycode);
int android_app_input_queue_depth(void);
int android_app_is_destroy_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_NDK_H */
