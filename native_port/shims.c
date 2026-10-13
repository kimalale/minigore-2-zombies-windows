#include "shims.h"
#include "watchdog.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <errno.h>
#include <windows.h>
#include <mmsystem.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <locale.h>

#ifdef CDECL
#undef CDECL
#endif
#define CDECL __attribute__((cdecl))

/* Bionic ctype tables. In Bionic (from OpenBSD) the exported symbols are
 * *pointers* to these tables:
 *     extern const char *_ctype_;
 *     extern const short *_tolower_tab_, *_toupper_tab_;
 * and inlined isX()/toupper()/tolower() read e.g. _toupper_tab_[c + 1]
 * through them. Exporting the arrays themselves made the game read the
 * first table entries as a pointer (0x0000FFFF) and crash, e.g. in
 * StateStore::addCharacterSelectButton when upper-casing a name. */
static unsigned char bionic_ctype_[257];
static short bionic_tolower_tab_[257];
static short bionic_toupper_tab_[257];
static const unsigned char *bionic_ctype_ptr = bionic_ctype_;
static const short *bionic_tolower_tab_ptr = bionic_tolower_tab_;
static const short *bionic_toupper_tab_ptr = bionic_toupper_tab_;

/* Bionic __sF: `extern FILE __sF[3]` - three Bionic FILE structs (0x54
 * bytes each), with stdout = &__sF[1] and stderr = &__sF[2] (= __sF +
 * 0xa8). It used to be exported as an array of three msvcrt FILE*s, so a
 * game write to stderr (PowerVR texture errors, the C++ terminate handler)
 * handed msvcrt a pointer past the end of that array. Now __sF is sized
 * like Bionic's and the stdio shims below translate pointers into it. */
#define BIONIC_FILE_SIZE 0x54
static uint8_t bionic_sF_[3 * BIONIC_FILE_SIZE] __attribute__((aligned(16)));

static int is_std_stream(void *fp) {
    uint8_t *p = (uint8_t *)fp;
    return p >= bionic_sF_ && p < bionic_sF_ + sizeof(bionic_sF_);
}

static FILE *host_file(void *fp) {
    if (is_std_stream(fp)) {
        switch (((uint8_t *)fp - bionic_sF_) / BIONIC_FILE_SIZE) {
            case 0: return stdin;
            case 1: return stdout;
            default: return stderr;
        }
    }
    return (FILE *)fp; /* opened by the game through fopen / fdopen */
}

static int CDECL shim_fclose(void *fp) { return is_std_stream(fp) ? 0 : fclose(host_file(fp)); }
static size_t CDECL shim_fread(void *b, size_t s, size_t n, void *fp) { return fread(b, s, n, host_file(fp)); }
static size_t CDECL shim_fwrite(const void *b, size_t s, size_t n, void *fp) { return fwrite(b, s, n, host_file(fp)); }
static int CDECL shim_fseek(void *fp, long off, int whence) { return fseek(host_file(fp), off, whence); }
static long CDECL shim_ftell(void *fp) { return ftell(host_file(fp)); }
static int CDECL shim_fputs(const char *str, void *fp) { return fputs(str, host_file(fp)); }
static int CDECL shim_fputc(int c, void *fp) { return fputc(c, host_file(fp)); }
static int CDECL shim_putc(int c, void *fp) { return putc(c, host_file(fp)); }
static int CDECL shim_getc(void *fp) { return getc(host_file(fp)); }
static int CDECL shim_ungetc(int c, void *fp) { return ungetc(c, host_file(fp)); }
static char * CDECL shim_fgets(char *b, int n, void *fp) { return fgets(b, n, host_file(fp)); }
static int CDECL shim_fflush(void *fp) { return fflush(fp ? host_file(fp) : NULL); }
static int CDECL shim_setvbuf(void *fp, char *buf, int mode, size_t size) {
    /* leave the host's stdio buffering (set up by log.c) alone */
    return is_std_stream(fp) ? 0 : setvbuf(host_file(fp), buf, mode, size);
}

/* Forward declarations of GL wrappers */
extern void CDECL wrap_glActiveTexture(unsigned int);
extern void CDECL wrap_glAttachShader(unsigned int, unsigned int);
extern void CDECL wrap_glBindAttribLocation(unsigned int, unsigned int, const char *);
extern void CDECL wrap_glBindBuffer(unsigned int, unsigned int);
extern void CDECL wrap_glBindFramebuffer(unsigned int, unsigned int);
extern void CDECL wrap_glBindTexture(unsigned int, unsigned int);
extern void CDECL wrap_glBlendEquationSeparate(unsigned int, unsigned int);
extern void CDECL wrap_glBlendFunc(unsigned int, unsigned int);
extern void CDECL wrap_glBlendFuncSeparate(unsigned int, unsigned int, unsigned int, unsigned int);
extern void CDECL wrap_glBufferData(unsigned int, ptrdiff_t, const void *, unsigned int);
extern void CDECL wrap_glBufferSubData(unsigned int, ptrdiff_t, ptrdiff_t, const void *);
extern unsigned int CDECL wrap_glCheckFramebufferStatus(unsigned int);
extern void CDECL wrap_glClear(unsigned int);
extern void CDECL wrap_glClearColor(float, float, float, float);
extern void CDECL wrap_glClearDepthf(float);
extern void CDECL wrap_glClearStencil(int);
extern void CDECL wrap_glColorMask(unsigned char, unsigned char, unsigned char, unsigned char);
extern void CDECL wrap_glCompileShader(unsigned int);
extern void CDECL wrap_glCompressedTexImage2D(unsigned int, int, unsigned int, int, int, int, int, const void *);
extern unsigned int CDECL wrap_glCreateProgram(void);
extern unsigned int CDECL wrap_glCreateShader(unsigned int);
extern void CDECL wrap_glCullFace(unsigned int);
extern void CDECL wrap_glDeleteBuffers(int, const unsigned int *);
extern void CDECL wrap_glDeleteFramebuffers(int, const unsigned int *);
extern void CDECL wrap_glDeleteTextures(int, const unsigned int *);
extern void CDECL wrap_glDepthFunc(unsigned int);
extern void CDECL wrap_glDepthMask(unsigned char);
extern void CDECL wrap_glDisable(unsigned int);
extern void CDECL wrap_glDisableVertexAttribArray(unsigned int);
extern void CDECL wrap_glDrawArrays(unsigned int, int, int);
extern void CDECL wrap_glDrawElements(unsigned int, int, unsigned int, const void *);
extern void CDECL wrap_glEnable(unsigned int);
extern void CDECL wrap_glEnableVertexAttribArray(unsigned int);
extern void CDECL wrap_glFramebufferTexture2D(unsigned int, unsigned int, unsigned int, unsigned int, int);
extern void CDECL wrap_glFrontFace(unsigned int);
extern void CDECL wrap_glGenBuffers(int, unsigned int *);
extern void CDECL wrap_glGenFramebuffers(int, unsigned int *);
extern void CDECL wrap_glGenTextures(int, unsigned int *);
extern void CDECL wrap_glGetActiveUniform(unsigned int, unsigned int, int, int *, int *, unsigned int *, char *);
extern unsigned int CDECL wrap_glGetError(void);
extern void CDECL wrap_glGetIntegerv(unsigned int, int *);
extern void CDECL wrap_glGetProgramInfoLog(unsigned int, int, int *, char *);
extern void CDECL wrap_glGetProgramiv(unsigned int, unsigned int, int *);
extern void CDECL wrap_glGetShaderInfoLog(unsigned int, int, int *, char *);
extern void CDECL wrap_glGetShaderiv(unsigned int, unsigned int, int *);
extern const unsigned char * CDECL wrap_glGetString(unsigned int);
extern int CDECL wrap_glGetUniformLocation(unsigned int, const char *);
extern void CDECL wrap_glLineWidth(float);
extern void CDECL wrap_glLinkProgram(unsigned int);
extern void CDECL wrap_glPixelStorei(unsigned int, int);
extern void CDECL wrap_glPolygonOffset(float, float);
extern void CDECL wrap_glScissor(int, int, int, int);
extern void CDECL wrap_glShaderSource(unsigned int, int, const char *const *, const int *);
extern void CDECL wrap_glTexImage2D(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
extern void CDECL wrap_glTexParameteri(unsigned int, unsigned int, int);
extern void CDECL wrap_glTexSubImage2D(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
extern void CDECL wrap_glUniform1f(int, float);
extern void CDECL wrap_glUniform1fv(int, int, const float *);
extern void CDECL wrap_glUniform1i(int, int);
extern void CDECL wrap_glUniform1iv(int, int, const int *);
extern void CDECL wrap_glUniform2fv(int, int, const float *);
extern void CDECL wrap_glUniform2iv(int, int, const int *);
extern void CDECL wrap_glUniform3fv(int, int, const float *);
extern void CDECL wrap_glUniform3iv(int, int, const int *);
extern void CDECL wrap_glUniform4fv(int, int, const float *);
extern void CDECL wrap_glUniform4iv(int, int, const int *);
extern void CDECL wrap_glUniformMatrix2fv(int, int, unsigned char, const float *);
extern void CDECL wrap_glUniformMatrix3fv(int, int, unsigned char, const float *);
extern void CDECL wrap_glUniformMatrix4fv(int, int, unsigned char, const float *);
extern void CDECL wrap_glUseProgram(unsigned int);
extern void CDECL wrap_glVertexAttribPointer(unsigned int, int, unsigned int, unsigned char, int, const void *);
extern void CDECL wrap_glViewport(int, int, int, int);

/* Forward declarations of EGL wrappers */
extern void * CDECL wrap_eglGetDisplay(void *);
extern uint32_t CDECL wrap_eglInitialize(void *, int *, int *);
extern uint32_t CDECL wrap_eglChooseConfig(void *, const int *, void **, int, int *);
extern uint32_t CDECL wrap_eglGetConfigAttrib(void *, void *, int, int *);
extern void * CDECL wrap_eglCreateWindowSurface(void *, void *, void *, const int *);
extern void * CDECL wrap_eglCreateContext(void *, void *, void *, const int *);
extern uint32_t CDECL wrap_eglMakeCurrent(void *, void *, void *, void *);
extern void * CDECL wrap_eglGetCurrentContext(void);
extern void * CDECL wrap_eglGetCurrentSurface(int);
extern uint32_t CDECL wrap_eglQuerySurface(void *, void *, int, int *);
extern uint32_t CDECL wrap_eglSwapBuffers(void *, void *);
extern int CDECL wrap_eglGetError(void);
extern uint32_t CDECL wrap_eglBindAPI(uint32_t);
extern uint32_t CDECL wrap_eglDestroyContext(void *, void *);
extern uint32_t CDECL wrap_eglDestroySurface(void *, void *);
extern uint32_t CDECL wrap_eglTerminate(void *);
extern void * CDECL wrap_eglGetProcAddress(const char *);

/* Forward declarations of OpenSL ES */
extern int32_t CDECL wrap_slCreateEngine(void **, uint32_t, void *, uint32_t, const void *, const uint32_t *);
extern const void *SL_IID_ENGINE;
extern const void *SL_IID_PLAY;
extern const void *SL_IID_BUFFERQUEUE;
extern const void *SL_IID_VOLUME;
extern const void *SL_IID_SEEK;
extern const void *SL_IID_PLAYBACKRATE;
extern const void *SL_IID_DYNAMICINTERFACEMANAGEMENT;
extern const void *SL_IID_EFFECTSEND;

/* Forward declarations of Android NDK */
extern void * CDECL wrap_AAssetManager_open(void *, const char *, int);
extern int CDECL wrap_AAsset_read(void *, void *, size_t);
extern off_t CDECL wrap_AAsset_seek(void *, off_t, int);
extern off_t CDECL wrap_AAsset_getLength(void *);
extern const void * CDECL wrap_AAsset_getBuffer(void *);
extern void CDECL wrap_AAsset_close(void *);
extern int CDECL wrap_AAsset_openFileDescriptor(void *, off_t *, off_t *);
extern void * CDECL wrap_AAssetManager_openDir(void *, const char *);
extern const char * CDECL wrap_AAssetDir_getNextFileName(void *);
extern void CDECL wrap_AAssetDir_close(void *);
extern void * CDECL wrap_AConfiguration_new(void);
extern void CDECL wrap_AConfiguration_delete(void *);
extern void CDECL wrap_AConfiguration_fromAssetManager(void *, void *);
extern void CDECL wrap_AConfiguration_getCountry(void *, char *);
extern void CDECL wrap_AConfiguration_getLanguage(void *, char *);
extern int32_t CDECL wrap_ANativeWindow_getWidth(void *);
extern int32_t CDECL wrap_ANativeWindow_getHeight(void *);
extern int32_t CDECL wrap_ANativeWindow_setBuffersGeometry(void *, int32_t, int32_t, int32_t);
extern void * CDECL wrap_ALooper_prepare(int);
extern int CDECL wrap_ALooper_addFd(void *, int, int, int, void *, void *);
extern int CDECL wrap_ALooper_pollAll(int, int *, int *, void **);
extern void CDECL wrap_AInputQueue_attachLooper(void *, void *, int, void *, void *);
extern void CDECL wrap_AInputQueue_detachLooper(void *);
extern int32_t CDECL wrap_AInputQueue_getEvent(void *, void **);
extern int32_t CDECL wrap_AInputQueue_preDispatchEvent(void *, void *);
extern void CDECL wrap_AInputQueue_finishEvent(void *, void *, int);
extern int32_t CDECL wrap_AInputEvent_getType(const void *);
extern int32_t CDECL wrap_AInputEvent_getDeviceId(const void *);
extern int32_t CDECL wrap_AInputEvent_getSource(const void *);
extern int32_t CDECL wrap_AMotionEvent_getAction(const void *);
extern int32_t CDECL wrap_AMotionEvent_getFlags(const void *);
extern int32_t CDECL wrap_AMotionEvent_getMetaState(const void *);
extern size_t CDECL wrap_AMotionEvent_getPointerCount(const void *);
extern int32_t CDECL wrap_AMotionEvent_getPointerId(const void *, size_t);
extern float CDECL wrap_AMotionEvent_getX(const void *, size_t);
extern float CDECL wrap_AMotionEvent_getY(const void *, size_t);
extern size_t CDECL wrap_AMotionEvent_getHistorySize(const void *);
extern float CDECL wrap_AMotionEvent_getHistoricalX(const void *, size_t, size_t);
extern float CDECL wrap_AMotionEvent_getHistoricalY(const void *, size_t, size_t);
extern int32_t CDECL wrap_AKeyEvent_getAction(const void *);
extern int32_t CDECL wrap_AKeyEvent_getKeyCode(const void *);
extern int32_t CDECL wrap_AKeyEvent_getFlags(const void *);
extern int32_t CDECL wrap_AKeyEvent_getMetaState(const void *);
extern void CDECL wrap_ANativeActivity_finish(void *);
extern int CDECL wrap_android_getCpuFamily(void);
extern uint64_t CDECL wrap_android_getCpuFeatures(void);
extern int CDECL wrap_android_getCpuCount(void);
extern void CDECL wrap_android_setCpu(int, uint64_t);
extern void CDECL wrap_app_dummy(void);
extern int8_t CDECL wrap_android_app_read_cmd(void *);
extern void CDECL wrap_android_app_pre_exec_cmd(void *, int8_t);
extern void CDECL wrap_android_app_post_exec_cmd(void *, int8_t);
extern int CDECL wrap___android_log_print(int, const char *, const char *, ...);

/* POSIX shims */
static int * CDECL shim___errno(void) {
    return _errno();
}

static void CDECL shim___stack_chk_fail(void) {
    printf("[FATAL] Stack check failed!\n");
    log_flush();
    ExitProcess(1);
}

static int CDECL shim_clock_gettime(int clock_id, struct timespec *ts) {
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    ts->tv_sec = (time_t)(count.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)(((count.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
    return 0;
}

static int CDECL shim_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (req) {
        DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
        Sleep(ms > 0 ? ms : 1);
    }
    return 0;
}

/* Pthread shims
 *
 * Bionic's pthread_mutex_t and pthread_cond_t are a single 32-bit word whose
 * static initializers are small constants (0 for normal mutexes and
 * condition variables, 0x4000 / 0x8000 for recursive / error-checking
 * mutexes). We keep a pointer to a host object in that word, created on
 * first use. It fits exactly, so neighbouring C++ fields are never touched,
 * and there is no global table that can fill up.
 *
 * (The previous fixed 4096-slot address map never released entries for
 * mutexes whose memory was freed without pthread_mutex_destroy. Once it
 * filled, every lock scanned all 4096 slots under a global lock and then
 * silently became a no-op, causing data races and stalls partway into a
 * play session.) */

#define HOST_MUTEX_MAGIC 0x3158544Du /* "MTX1" */
#define HOST_COND_MAGIC  0x31444E43u /* "CND1" */

/* Upper bound on a condition wait before re-checking. POSIX allows spurious
 * wakeups, so this only guards against lost wakeups in game code. */
#define COND_WAIT_SLICE_MS 50

typedef struct {
    uint32_t magic;
    CRITICAL_SECTION cs;
} HostMutex;

typedef struct {
    uint32_t magic;
    CONDITION_VARIABLE cv;
} HostCond;

static volatile LONG g_mutex_count = 0;
static volatile LONG g_cond_count = 0;

/* Values below 64K are Bionic initializer constants, never our pointers. */
static int is_host_ptr(LONG v) {
    return (uint32_t)v >= 0x10000u;
}

static HostMutex *get_host_mutex(void *mutex) {
    volatile LONG *slot = (volatile LONG *)mutex;
    for (;;) {
        LONG v = *slot;
        if (is_host_ptr(v) && ((HostMutex *)(uintptr_t)v)->magic == HOST_MUTEX_MAGIC) {
            return (HostMutex *)(uintptr_t)v;
        }
        HostMutex *m = (HostMutex *)calloc(1, sizeof(HostMutex));
        InitializeCriticalSection(&m->cs);
        m->magic = HOST_MUTEX_MAGIC;
        if (InterlockedCompareExchange(slot, (LONG)(uintptr_t)m, v) == v) {
            InterlockedIncrement(&g_mutex_count);
            return m;
        }
        /* Another thread initialised it first */
        DeleteCriticalSection(&m->cs);
        free(m);
    }
}

static HostCond *get_host_cond(void *cond) {
    volatile LONG *slot = (volatile LONG *)cond;
    for (;;) {
        LONG v = *slot;
        if (is_host_ptr(v) && ((HostCond *)(uintptr_t)v)->magic == HOST_COND_MAGIC) {
            return (HostCond *)(uintptr_t)v;
        }
        HostCond *c = (HostCond *)calloc(1, sizeof(HostCond));
        InitializeConditionVariable(&c->cv);
        c->magic = HOST_COND_MAGIC;
        if (InterlockedCompareExchange(slot, (LONG)(uintptr_t)c, v) == v) {
            InterlockedIncrement(&g_cond_count);
            return c;
        }
        free(c);
    }
}

static int CDECL shim_pthread_mutex_init(void *mutex, const void *attr) {
    /* Created lazily on first lock; CRITICAL_SECTIONs are always recursive */
    if (mutex) *(volatile LONG *)mutex = 0;
    return 0;
}

static int CDECL shim_pthread_mutex_destroy(void *mutex) {
    if (!mutex) return 0;
    LONG v = InterlockedExchange((volatile LONG *)mutex, 0);
    if (is_host_ptr(v)) {
        HostMutex *m = (HostMutex *)(uintptr_t)v;
        if (m->magic == HOST_MUTEX_MAGIC) {
            m->magic = 0;
            DeleteCriticalSection(&m->cs);
            free(m);
            InterlockedDecrement(&g_mutex_count);
        }
    }
    return 0;
}

static int CDECL shim_pthread_mutex_lock(void *mutex) {
    if (!mutex) return 0;
    EnterCriticalSection(&get_host_mutex(mutex)->cs);
    return 0;
}

static int CDECL shim_pthread_mutex_unlock(void *mutex) {
    if (!mutex) return 0;
    LeaveCriticalSection(&get_host_mutex(mutex)->cs);
    return 0;
}

static int CDECL shim_pthread_cond_init(void *cv, const void *attr) {
    if (cv) *(volatile LONG *)cv = 0;
    return 0;
}

static int CDECL shim_pthread_cond_destroy(void *cv) {
    if (!cv) return 0;
    LONG v = InterlockedExchange((volatile LONG *)cv, 0);
    if (is_host_ptr(v)) {
        HostCond *c = (HostCond *)(uintptr_t)v;
        if (c->magic == HOST_COND_MAGIC) {
            c->magic = 0;
            free(c);
            InterlockedDecrement(&g_cond_count);
        }
    }
    return 0;
}

static int CDECL shim_pthread_cond_wait(void *cv, void *mutex) {
    if (!cv || !mutex) return 0;
    /* Previously this was unlock + Sleep(1) + lock, which with the default
     * 15.6 ms timer resolution added a frame of latency to every hand-off
     * between game threads. */
    SleepConditionVariableCS(&get_host_cond(cv)->cv, &get_host_mutex(mutex)->cs, COND_WAIT_SLICE_MS);
    return 0;
}

static int CDECL shim_pthread_cond_broadcast(void *cv) {
    if (cv) WakeAllConditionVariable(&get_host_cond(cv)->cv);
    return 0;
}

void shims_get_sync_stats(long *mutexes, long *conds) {
    *mutexes = g_mutex_count;
    *conds = g_cond_count;
}

/* Thread tracking */
#define MAX_THREADS 64
typedef struct {
    DWORD tid;
    HANDLE handle;
    int in_use;
} ThreadInfo;

static ThreadInfo g_threads[MAX_THREADS];
static CRITICAL_SECTION g_thread_lock;
static volatile LONG g_thread_lock_inited = 0;

static void init_thread_lock(void) {
    if (InterlockedCompareExchange(&g_thread_lock_inited, 1, 0) == 0) {
        InitializeCriticalSection(&g_thread_lock);
        memset(g_threads, 0, sizeof(g_threads));
    }
}

static void register_thread(DWORD tid, HANDLE handle) {
    init_thread_lock();
    EnterCriticalSection(&g_thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].in_use) {
            g_threads[i].tid = tid;
            g_threads[i].handle = handle;
            g_threads[i].in_use = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_thread_lock);
}

static HANDLE get_thread_handle_by_tid(DWORD tid) {
    init_thread_lock();
    EnterCriticalSection(&g_thread_lock);
    HANDLE h = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].in_use && g_threads[i].tid == tid) {
            h = g_threads[i].handle;
            break;
        }
    }
    LeaveCriticalSection(&g_thread_lock);
    return h;
}

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} ThreadBridge;

static DWORD WINAPI win32_thread_proc(LPVOID lpParam) {
    ThreadBridge *tb = (ThreadBridge *)lpParam;
    void *(*fn)(void *) = tb->start_routine;
    void *arg = tb->arg;
    free(tb);
    
    /* Commit thread stack upfront */
    MEMORY_BASIC_INFORMATION mbi;
    memset(&mbi, 0, sizeof(mbi));
    char *p = (char *)&mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi))) {
        if (mbi.AllocationBase) {
            SIZE_T size_to_commit = (SIZE_T)(p - (char *)mbi.AllocationBase);
            VirtualAlloc(mbi.AllocationBase, size_to_commit, MEM_COMMIT, PAGE_READWRITE);
        }
    }
    
    fn(arg);
    return 0;
}

static int CDECL shim_pthread_create(HANDLE *thread, const void *attr, void *(*start_routine)(void *), void *arg) {
    ThreadBridge *tb = (ThreadBridge *)malloc(sizeof(ThreadBridge));
    tb->start_routine = start_routine;
    tb->arg = arg;
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, win32_thread_proc, tb, 0, &tid);
    if (h) {
        register_thread(tid, h);
        watchdog_register_thread(h, "pthread");
        *thread = h;
        return 0;
    }
    return -1;
}

static int CDECL shim_pthread_join(HANDLE thread, void **retval) {
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        init_thread_lock();
        EnterCriticalSection(&g_thread_lock);
        for (int i = 0; i < MAX_THREADS; i++) {
            if (g_threads[i].in_use && g_threads[i].handle == thread) {
                g_threads[i].in_use = 0;
                break;
            }
        }
        LeaveCriticalSection(&g_thread_lock);
    }
    return 0;
}

static HANDLE CDECL shim_pthread_self(void) {
    DWORD tid = GetCurrentThreadId();
    HANDLE h = get_thread_handle_by_tid(tid);
    if (!h) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        register_thread(tid, h);
    }
    return h;
}

static void CDECL shim_pthread_exit(void *retval) {
    ExitThread(0);
}

/* 0 = not run (PTHREAD_ONCE_INIT), 1 = running, 2 = done. Other threads
 * wait until the routine has finished instead of racing past it. */
static int CDECL shim_pthread_once(int *once_control, void (*init_routine)(void)) {
    volatile LONG *ctl = (volatile LONG *)once_control;
    if (*ctl == 2) return 0;
    if (InterlockedCompareExchange(ctl, 1, 0) == 0) {
        init_routine();
        InterlockedExchange(ctl, 2);
        return 0;
    }
    while (*ctl != 2) Sleep(0);
    return 0;
}

static int CDECL shim_pthread_key_create(DWORD *key, void (*destructor)(void *)) {
    *key = TlsAlloc();
    return (*key != TLS_OUT_OF_INDEXES) ? 0 : -1;
}

static int CDECL shim_pthread_key_delete(DWORD key) {
    return TlsFree(key) ? 0 : -1;
}

static void * CDECL shim_pthread_getspecific(DWORD key) {
    return TlsGetValue(key);
}

static int CDECL shim_pthread_setspecific(DWORD key, const void *value) {
    return TlsSetValue(key, (LPVOID)value) ? 0 : -1;
}

static int CDECL shim_pthread_attr_init(void *attr) { return 0; }
static int CDECL shim_pthread_attr_setdetachstate(void *attr, int state) { return 0; }
static int CDECL shim_sched_yield(void) { Sleep(0); return 0; }

static long CDECL shim_lrand48(void) { return (long)(rand() & 0x7fffffff); }
static void CDECL shim_srand48(long seed) { srand((unsigned int)seed); }

static void * CDECL shim_memalign(size_t alignment, size_t size) {
    return malloc(size);
}

static float CDECL wrap_AMotionEvent_getAxisValue(const void *event, int32_t axis, size_t pointer_index) {
    return 0.0f;
}

static float CDECL wrap_AMotionEvent_getHistoricalAxisValue(const void *event, int32_t axis, size_t pointer_index, size_t history_pos) {
    return 0.0f;
}

static void * CDECL shim_dlsym(void *handle, const char *symbol) {
    if (!symbol) return NULL;
    if (strcmp(symbol, "AMotionEvent_getAxisValue") == 0) return (void *)wrap_AMotionEvent_getAxisValue;
    if (strcmp(symbol, "AMotionEvent_getHistoricalAxisValue") == 0) return (void *)wrap_AMotionEvent_getHistoricalAxisValue;
    return NULL;
}

static int CDECL shim_dl_iterate_phdr(void *cb, void *data) { return 0; }
static int CDECL shim___cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) { return 0; }
static void CDECL shim___cxa_finalize(void *dso_handle) {}

static void CDECL shim_abort(void) {
    printf("[SHIM] abort() called!\n");
    log_flush();
    ExitProcess(1);
}

static void CDECL shim_exit(int code) {
    printf("[SHIM] exit(%d) called!\n", code);
    log_flush();
    ExitProcess(code);
}

static void CDECL shim_google_potentially_blocking_region_begin(void) {}
static void CDECL shim_google_potentially_blocking_region_end(void) {}

static int CDECL shim_syscall(int number, ...) { return -1; }
static int CDECL shim_ioctl(int fd, unsigned long request, ...) { return -1; }
static int CDECL shim_pipe(int pipefd[2]) { return _pipe(pipefd, 4096, _O_BINARY); }
static int CDECL shim_poll(void *fds, unsigned long nfds, int timeout) { return 0; }
static int CDECL shim_writev(int fd, const void *iov, int iovcnt) { return -1; }

void shims_init(void) {
    /* 1 ms scheduler resolution so nanosleep and condition waits are not
     * rounded up to the default 15.6 ms tick */
    timeBeginPeriod(1);

    /* Initialize Bionic ctype tables */
    for (int i = 0; i < 256; i++) {
        unsigned char mask = 0;
        if (isupper(i)) mask |= 0x01;
        if (islower(i)) mask |= 0x02;
        if (isdigit(i)) mask |= 0x04;
        if (isspace(i)) mask |= 0x08;
        if (ispunct(i)) mask |= 0x10;
        if (iscntrl(i)) mask |= 0x20;
        if (isxdigit(i)) mask |= 0x40; /* _X */
        if (isblank(i)) mask |= 0x80;  /* _B */
        bionic_ctype_[i + 1] = mask;
        bionic_tolower_tab_[i + 1] = (short)tolower(i);
        bionic_toupper_tab_[i + 1] = (short)toupper(i);
    }
    bionic_ctype_[0] = 0;
    bionic_tolower_tab_[0] = -1;
    bionic_toupper_tab_[0] = -1;
    
    
    init_thread_lock();
}

/* Master symbol table mapping all symbols needed by libminigore2.so */
static const host_symbol_t g_host_symbols[] = {
    /* Bionic / Libc runtime */
    { "__android_log_print", (uintptr_t)wrap___android_log_print },
    { "__cxa_atexit",        (uintptr_t)shim___cxa_atexit },
    { "__cxa_finalize",      (uintptr_t)shim___cxa_finalize },
    { "__errno",             (uintptr_t)shim___errno },
    { "__google_potentially_blocking_region_begin", (uintptr_t)shim_google_potentially_blocking_region_begin },
    { "__google_potentially_blocking_region_end",   (uintptr_t)shim_google_potentially_blocking_region_end },
    { "__sF",                (uintptr_t)bionic_sF_ },
    { "__stack_chk_fail",    (uintptr_t)shim___stack_chk_fail },
    { "_ctype_",             (uintptr_t)&bionic_ctype_ptr },
    { "_tolower_tab_",       (uintptr_t)&bionic_tolower_tab_ptr },
    { "_toupper_tab_",       (uintptr_t)&bionic_toupper_tab_ptr },
    { "abort",               (uintptr_t)shim_abort },
    { "exit",                (uintptr_t)shim_exit },
    { "calloc",              (uintptr_t)calloc },
    { "malloc",              (uintptr_t)malloc },
    { "free",                (uintptr_t)free },
    { "realloc",             (uintptr_t)realloc },
    { "memalign",            (uintptr_t)shim_memalign },
    { "memcpy",              (uintptr_t)memcpy },
    { "memmove",             (uintptr_t)memmove },
    { "memset",              (uintptr_t)memset },
    { "memchr",              (uintptr_t)memchr },
    { "memcmp",              (uintptr_t)memcmp },
    { "strlen",              (uintptr_t)strlen },
    { "strcpy",              (uintptr_t)strcpy },
    { "strncpy",             (uintptr_t)strncpy },
    { "strcat",              (uintptr_t)strcat },
    { "strchr",              (uintptr_t)strchr },
    { "strstr",              (uintptr_t)strstr },
    { "strcmp",              (uintptr_t)strcmp },
    { "strncmp",             (uintptr_t)strncmp },
    { "strtod",              (uintptr_t)strtod },
    { "strtol",              (uintptr_t)strtol },
    { "sprintf",             (uintptr_t)sprintf },
    { "sscanf",              (uintptr_t)sscanf },
    { "vsnprintf",           (uintptr_t)vsnprintf },
    { "vsprintf",            (uintptr_t)vsprintf },
    { "strerror",            (uintptr_t)strerror },
    { "strxfrm",             (uintptr_t)strxfrm },
    { "strcoll",             (uintptr_t)strcoll },
    { "atoi",                (uintptr_t)atoi },
    { "qsort",               (uintptr_t)qsort },
    
    /* Math */
    { "sin",                 (uintptr_t)sin },
    { "cos",                 (uintptr_t)cos },
    { "tan",                 (uintptr_t)tan },
    { "sinf",                (uintptr_t)sinf },
    { "cosf",                (uintptr_t)cosf },
    { "tanf",                (uintptr_t)tanf },
    { "asin",                (uintptr_t)asin },
    { "asinf",               (uintptr_t)asinf },
    { "acos",                (uintptr_t)acos },
    { "atan",                (uintptr_t)atan },
    { "atanf",               (uintptr_t)atanf },
    { "atan2",               (uintptr_t)atan2 },
    { "atan2f",              (uintptr_t)atan2f },
    { "sqrt",                (uintptr_t)sqrt },
    { "sqrtf",               (uintptr_t)sqrtf },
    { "exp",                 (uintptr_t)exp },
    { "log",                 (uintptr_t)log },
    { "logf",                (uintptr_t)logf },
    { "pow",                 (uintptr_t)pow },
    { "powf",                (uintptr_t)powf },
    { "floor",               (uintptr_t)floor },
    { "floorf",              (uintptr_t)floorf },
    { "ceil",                (uintptr_t)ceil },
    { "fmod",                (uintptr_t)fmod },
    { "fmodf",               (uintptr_t)fmodf },
    { "ldexp",               (uintptr_t)ldexp },
    { "rint",                (uintptr_t)rint },
    { "rintf",               (uintptr_t)rintf },
    { "lrand48",             (uintptr_t)shim_lrand48 },
    { "srand48",             (uintptr_t)shim_srand48 },
    
    /* File IO */
    { "fopen",               (uintptr_t)fopen },
    { "fclose",              (uintptr_t)shim_fclose },
    { "fread",               (uintptr_t)shim_fread },
    { "fwrite",              (uintptr_t)shim_fwrite },
    { "fseek",               (uintptr_t)shim_fseek },
    { "ftell",               (uintptr_t)shim_ftell },
    { "fputs",               (uintptr_t)shim_fputs },
    { "fputc",               (uintptr_t)shim_fputc },
    { "fgets",               (uintptr_t)shim_fgets },
    { "fflush",              (uintptr_t)shim_fflush },
    { "setvbuf",             (uintptr_t)shim_setvbuf },
    { "remove",              (uintptr_t)remove },
    { "rename",              (uintptr_t)rename },
    { "open",                (uintptr_t)open },
    { "close",               (uintptr_t)close },
    { "read",                (uintptr_t)read },
    { "write",               (uintptr_t)write },
    { "lseek",               (uintptr_t)lseek },
    { "fstat",               (uintptr_t)fstat },
    { "fdopen",              (uintptr_t)fdopen },
    { "getc",                (uintptr_t)shim_getc },
    { "putc",                (uintptr_t)shim_putc },
    { "ungetc",              (uintptr_t)shim_ungetc },
    { "ioctl",               (uintptr_t)shim_ioctl },
    { "pipe",                (uintptr_t)shim_pipe },
    { "poll",                (uintptr_t)shim_poll },
    { "writev",              (uintptr_t)shim_writev },
    { "syscall",             (uintptr_t)shim_syscall },
    
    /* Time */
    { "time",                (uintptr_t)time },
    { "clock",               (uintptr_t)clock },
    { "clock_gettime",       (uintptr_t)shim_clock_gettime },
    { "localtime",           (uintptr_t)localtime },
    { "strftime",            (uintptr_t)strftime },
    { "mktime",              (uintptr_t)mktime },
    { "nanosleep",           (uintptr_t)shim_nanosleep },
    { "difftime",            (uintptr_t)difftime },
    
    /* Pthreads */
    { "pthread_create",      (uintptr_t)shim_pthread_create },
    { "pthread_join",        (uintptr_t)shim_pthread_join },
    { "pthread_self",        (uintptr_t)shim_pthread_self },
    { "pthread_exit",        (uintptr_t)shim_pthread_exit },
    { "pthread_mutex_init",  (uintptr_t)shim_pthread_mutex_init },
    { "pthread_mutex_destroy",(uintptr_t)shim_pthread_mutex_destroy },
    { "pthread_mutex_lock",  (uintptr_t)shim_pthread_mutex_lock },
    { "pthread_mutex_unlock",(uintptr_t)shim_pthread_mutex_unlock },
    { "pthread_cond_init",   (uintptr_t)shim_pthread_cond_init },
    { "pthread_cond_destroy",(uintptr_t)shim_pthread_cond_destroy },
    { "pthread_cond_wait",   (uintptr_t)shim_pthread_cond_wait },
    { "pthread_cond_broadcast",(uintptr_t)shim_pthread_cond_broadcast },
    { "pthread_once",        (uintptr_t)shim_pthread_once },
    { "pthread_key_create",  (uintptr_t)shim_pthread_key_create },
    { "pthread_key_delete",  (uintptr_t)shim_pthread_key_delete },
    { "pthread_getspecific", (uintptr_t)shim_pthread_getspecific },
    { "pthread_setspecific", (uintptr_t)shim_pthread_setspecific },
    { "pthread_attr_init",   (uintptr_t)shim_pthread_attr_init },
    { "pthread_attr_setdetachstate", (uintptr_t)shim_pthread_attr_setdetachstate },
    { "sched_yield",         (uintptr_t)shim_sched_yield },
    
    /* Wchar / Ctype */
    { "btowc",               (uintptr_t)btowc },
    { "getwc",               (uintptr_t)getwc },
    { "putwc",               (uintptr_t)putwc },
    { "ungetwc",             (uintptr_t)ungetwc },
    { "iswctype",            (uintptr_t)iswctype },
    { "wctype",              (uintptr_t)wctype },
    { "mbrtowc",             (uintptr_t)mbrtowc },
    { "wcrtomb",             (uintptr_t)wcrtomb },
    { "wcscoll",             (uintptr_t)wcscoll },
    { "wcsftime",            (uintptr_t)wcsftime },
    { "wcslen",              (uintptr_t)wcslen },
    { "wcsxfrm",             (uintptr_t)wcsxfrm },
    { "wctob",               (uintptr_t)wctob },
    { "wmemchr",             (uintptr_t)wmemchr },
    { "wmemcmp",             (uintptr_t)wmemcmp },
    { "wmemcpy",             (uintptr_t)wmemcpy },
    { "wmemmove",            (uintptr_t)wmemmove },
    { "wmemset",             (uintptr_t)wmemset },
    { "towlower",            (uintptr_t)towlower },
    { "towupper",            (uintptr_t)towupper },
    { "setlocale",           (uintptr_t)setlocale },
    
    /* Dynamic linking */
    { "dlsym",               (uintptr_t)shim_dlsym },
    { "dl_iterate_phdr",     (uintptr_t)shim_dl_iterate_phdr },
    
    /* Android NDK APIs */
    { "AAsset_close",        (uintptr_t)wrap_AAsset_close },
    { "AAsset_getBuffer",    (uintptr_t)wrap_AAsset_getBuffer },
    { "AAsset_getLength",    (uintptr_t)wrap_AAsset_getLength },
    { "AAsset_openFileDescriptor", (uintptr_t)wrap_AAsset_openFileDescriptor },
    { "AAsset_read",         (uintptr_t)wrap_AAsset_read },
    { "AAsset_seek",         (uintptr_t)wrap_AAsset_seek },
    { "AAssetDir_close",     (uintptr_t)wrap_AAssetDir_close },
    { "AAssetDir_getNextFileName", (uintptr_t)wrap_AAssetDir_getNextFileName },
    { "AAssetManager_open",  (uintptr_t)wrap_AAssetManager_open },
    { "AAssetManager_openDir",(uintptr_t)wrap_AAssetManager_openDir },
    { "AConfiguration_delete",(uintptr_t)wrap_AConfiguration_delete },
    { "AConfiguration_fromAssetManager", (uintptr_t)wrap_AConfiguration_fromAssetManager },
    { "AConfiguration_getCountry", (uintptr_t)wrap_AConfiguration_getCountry },
    { "AConfiguration_getLanguage",(uintptr_t)wrap_AConfiguration_getLanguage },
    { "AConfiguration_new",  (uintptr_t)wrap_AConfiguration_new },
    { "AInputEvent_getDeviceId", (uintptr_t)wrap_AInputEvent_getDeviceId },
    { "AInputEvent_getSource", (uintptr_t)wrap_AInputEvent_getSource },
    { "AInputEvent_getType", (uintptr_t)wrap_AInputEvent_getType },
    { "AInputQueue_attachLooper", (uintptr_t)wrap_AInputQueue_attachLooper },
    { "AInputQueue_detachLooper", (uintptr_t)wrap_AInputQueue_detachLooper },
    { "AInputQueue_finishEvent", (uintptr_t)wrap_AInputQueue_finishEvent },
    { "AInputQueue_getEvent",(uintptr_t)wrap_AInputQueue_getEvent },
    { "AInputQueue_preDispatchEvent", (uintptr_t)wrap_AInputQueue_preDispatchEvent },
    { "AKeyEvent_getAction", (uintptr_t)wrap_AKeyEvent_getAction },
    { "AKeyEvent_getFlags",  (uintptr_t)wrap_AKeyEvent_getFlags },
    { "AKeyEvent_getKeyCode",(uintptr_t)wrap_AKeyEvent_getKeyCode },
    { "AKeyEvent_getMetaState", (uintptr_t)wrap_AKeyEvent_getMetaState },
    { "ALooper_addFd",       (uintptr_t)wrap_ALooper_addFd },
    { "ALooper_pollAll",     (uintptr_t)wrap_ALooper_pollAll },
    { "ALooper_prepare",     (uintptr_t)wrap_ALooper_prepare },
    { "AMotionEvent_getAction", (uintptr_t)wrap_AMotionEvent_getAction },
    { "AMotionEvent_getFlags",(uintptr_t)wrap_AMotionEvent_getFlags },
    { "AMotionEvent_getHistoricalX", (uintptr_t)wrap_AMotionEvent_getHistoricalX },
    { "AMotionEvent_getHistoricalY", (uintptr_t)wrap_AMotionEvent_getHistoricalY },
    { "AMotionEvent_getHistorySize", (uintptr_t)wrap_AMotionEvent_getHistorySize },
    { "AMotionEvent_getMetaState", (uintptr_t)wrap_AMotionEvent_getMetaState },
    { "AMotionEvent_getPointerCount", (uintptr_t)wrap_AMotionEvent_getPointerCount },
    { "AMotionEvent_getPointerId", (uintptr_t)wrap_AMotionEvent_getPointerId },
    { "AMotionEvent_getX",   (uintptr_t)wrap_AMotionEvent_getX },
    { "AMotionEvent_getY",   (uintptr_t)wrap_AMotionEvent_getY },
    { "ANativeActivity_finish", (uintptr_t)wrap_ANativeActivity_finish },
    { "ANativeWindow_getHeight", (uintptr_t)wrap_ANativeWindow_getHeight },
    { "ANativeWindow_getWidth",  (uintptr_t)wrap_ANativeWindow_getWidth },
    { "ANativeWindow_setBuffersGeometry", (uintptr_t)wrap_ANativeWindow_setBuffersGeometry },
    { "android_app_post_exec_cmd", (uintptr_t)wrap_android_app_post_exec_cmd },
    { "android_app_pre_exec_cmd",  (uintptr_t)wrap_android_app_pre_exec_cmd },
    { "android_app_read_cmd",      (uintptr_t)wrap_android_app_read_cmd },
    { "android_getCpuCount",       (uintptr_t)wrap_android_getCpuCount },
    { "android_getCpuFamily",      (uintptr_t)wrap_android_getCpuFamily },
    { "android_getCpuFeatures",    (uintptr_t)wrap_android_getCpuFeatures },
    { "android_setCpu",            (uintptr_t)wrap_android_setCpu },
    { "app_dummy",                 (uintptr_t)wrap_app_dummy },
    
    /* EGL APIs */
    { "eglBindAPI",                (uintptr_t)wrap_eglBindAPI },
    { "eglChooseConfig",           (uintptr_t)wrap_eglChooseConfig },
    { "eglCreateContext",          (uintptr_t)wrap_eglCreateContext },
    { "eglCreateWindowSurface",    (uintptr_t)wrap_eglCreateWindowSurface },
    { "eglDestroyContext",         (uintptr_t)wrap_eglDestroyContext },
    { "eglDestroySurface",         (uintptr_t)wrap_eglDestroySurface },
    { "eglGetConfigAttrib",        (uintptr_t)wrap_eglGetConfigAttrib },
    { "eglGetCurrentContext",      (uintptr_t)wrap_eglGetCurrentContext },
    { "eglGetCurrentSurface",      (uintptr_t)wrap_eglGetCurrentSurface },
    { "eglGetDisplay",             (uintptr_t)wrap_eglGetDisplay },
    { "eglGetError",               (uintptr_t)wrap_eglGetError },
    { "eglGetProcAddress",         (uintptr_t)wrap_eglGetProcAddress },
    { "eglInitialize",             (uintptr_t)wrap_eglInitialize },
    { "eglMakeCurrent",            (uintptr_t)wrap_eglMakeCurrent },
    { "eglQuerySurface",           (uintptr_t)wrap_eglQuerySurface },
    { "eglSwapBuffers",            (uintptr_t)wrap_eglSwapBuffers },
    { "eglTerminate",              (uintptr_t)wrap_eglTerminate },
    
    /* OpenGL ES 2.0 APIs */
    { "glActiveTexture",           (uintptr_t)wrap_glActiveTexture },
    { "glAttachShader",            (uintptr_t)wrap_glAttachShader },
    { "glBindAttribLocation",      (uintptr_t)wrap_glBindAttribLocation },
    { "glBindBuffer",              (uintptr_t)wrap_glBindBuffer },
    { "glBindFramebuffer",         (uintptr_t)wrap_glBindFramebuffer },
    { "glBindTexture",             (uintptr_t)wrap_glBindTexture },
    { "glBlendEquationSeparate",   (uintptr_t)wrap_glBlendEquationSeparate },
    { "glBlendFunc",               (uintptr_t)wrap_glBlendFunc },
    { "glBlendFuncSeparate",       (uintptr_t)wrap_glBlendFuncSeparate },
    { "glBufferData",              (uintptr_t)wrap_glBufferData },
    { "glBufferSubData",           (uintptr_t)wrap_glBufferSubData },
    { "glCheckFramebufferStatus",  (uintptr_t)wrap_glCheckFramebufferStatus },
    { "glClear",                   (uintptr_t)wrap_glClear },
    { "glClearColor",              (uintptr_t)wrap_glClearColor },
    { "glClearDepthf",             (uintptr_t)wrap_glClearDepthf },
    { "glClearStencil",            (uintptr_t)wrap_glClearStencil },
    { "glColorMask",               (uintptr_t)wrap_glColorMask },
    { "glCompileShader",           (uintptr_t)wrap_glCompileShader },
    { "glCompressedTexImage2D",    (uintptr_t)wrap_glCompressedTexImage2D },
    { "glCreateProgram",           (uintptr_t)wrap_glCreateProgram },
    { "glCreateShader",            (uintptr_t)wrap_glCreateShader },
    { "glCullFace",                (uintptr_t)wrap_glCullFace },
    { "glDeleteBuffers",           (uintptr_t)wrap_glDeleteBuffers },
    { "glDeleteFramebuffers",      (uintptr_t)wrap_glDeleteFramebuffers },
    { "glDeleteTextures",          (uintptr_t)wrap_glDeleteTextures },
    { "glDepthFunc",               (uintptr_t)wrap_glDepthFunc },
    { "glDepthMask",               (uintptr_t)wrap_glDepthMask },
    { "glDisable",                 (uintptr_t)wrap_glDisable },
    { "glDisableVertexAttribArray",(uintptr_t)wrap_glDisableVertexAttribArray },
    { "glDrawArrays",              (uintptr_t)wrap_glDrawArrays },
    { "glDrawElements",            (uintptr_t)wrap_glDrawElements },
    { "glEnable",                  (uintptr_t)wrap_glEnable },
    { "glEnableVertexAttribArray", (uintptr_t)wrap_glEnableVertexAttribArray },
    { "glFramebufferTexture2D",    (uintptr_t)wrap_glFramebufferTexture2D },
    { "glFrontFace",               (uintptr_t)wrap_glFrontFace },
    { "glGenBuffers",              (uintptr_t)wrap_glGenBuffers },
    { "glGenFramebuffers",         (uintptr_t)wrap_glGenFramebuffers },
    { "glGenTextures",             (uintptr_t)wrap_glGenTextures },
    { "glGetActiveUniform",        (uintptr_t)wrap_glGetActiveUniform },
    { "glGetError",                (uintptr_t)wrap_glGetError },
    { "glGetIntegerv",             (uintptr_t)wrap_glGetIntegerv },
    { "glGetProgramInfoLog",       (uintptr_t)wrap_glGetProgramInfoLog },
    { "glGetProgramiv",            (uintptr_t)wrap_glGetProgramiv },
    { "glGetShaderInfoLog",        (uintptr_t)wrap_glGetShaderInfoLog },
    { "glGetShaderiv",             (uintptr_t)wrap_glGetShaderiv },
    { "glGetString",               (uintptr_t)wrap_glGetString },
    { "glGetUniformLocation",      (uintptr_t)wrap_glGetUniformLocation },
    { "glLineWidth",               (uintptr_t)wrap_glLineWidth },
    { "glLinkProgram",             (uintptr_t)wrap_glLinkProgram },
    { "glPixelStorei",             (uintptr_t)wrap_glPixelStorei },
    { "glPolygonOffset",           (uintptr_t)wrap_glPolygonOffset },
    { "glScissor",                 (uintptr_t)wrap_glScissor },
    { "glShaderSource",            (uintptr_t)wrap_glShaderSource },
    { "glTexImage2D",              (uintptr_t)wrap_glTexImage2D },
    { "glTexParameteri",          (uintptr_t)wrap_glTexParameteri },
    { "glTexSubImage2D",           (uintptr_t)wrap_glTexSubImage2D },
    { "glUniform1f",               (uintptr_t)wrap_glUniform1f },
    { "glUniform1fv",              (uintptr_t)wrap_glUniform1fv },
    { "glUniform1i",               (uintptr_t)wrap_glUniform1i },
    { "glUniform1iv",              (uintptr_t)wrap_glUniform1iv },
    { "glUniform2fv",              (uintptr_t)wrap_glUniform2fv },
    { "glUniform2iv",              (uintptr_t)wrap_glUniform2iv },
    { "glUniform3fv",              (uintptr_t)wrap_glUniform3fv },
    { "glUniform3iv",              (uintptr_t)wrap_glUniform3iv },
    { "glUniform4fv",              (uintptr_t)wrap_glUniform4fv },
    { "glUniform4iv",              (uintptr_t)wrap_glUniform4iv },
    { "glUniformMatrix2fv",        (uintptr_t)wrap_glUniformMatrix2fv },
    { "glUniformMatrix3fv",        (uintptr_t)wrap_glUniformMatrix3fv },
    { "glUniformMatrix4fv",        (uintptr_t)wrap_glUniformMatrix4fv },
    { "glUseProgram",              (uintptr_t)wrap_glUseProgram },
    { "glVertexAttribPointer",     (uintptr_t)wrap_glVertexAttribPointer },
    { "glViewport",                (uintptr_t)wrap_glViewport },
    
    /* OpenSL ES */
    { "slCreateEngine",            (uintptr_t)wrap_slCreateEngine },
    { "SL_IID_BUFFERQUEUE",        (uintptr_t)&SL_IID_BUFFERQUEUE },
    { "SL_IID_DYNAMICINTERFACEMANAGEMENT", (uintptr_t)&SL_IID_DYNAMICINTERFACEMANAGEMENT },
    { "SL_IID_EFFECTSEND",         (uintptr_t)&SL_IID_EFFECTSEND },
    { "SL_IID_ENGINE",             (uintptr_t)&SL_IID_ENGINE },
    { "SL_IID_PLAY",               (uintptr_t)&SL_IID_PLAY },
    { "SL_IID_PLAYBACKRATE",       (uintptr_t)&SL_IID_PLAYBACKRATE },
    { "SL_IID_SEEK",               (uintptr_t)&SL_IID_SEEK },
    { "SL_IID_VOLUME",             (uintptr_t)&SL_IID_VOLUME }
};

const host_symbol_t *shims_get_table(size_t *count) {
    if (count) {
        *count = sizeof(g_host_symbols) / sizeof(g_host_symbols[0]);
    }
    return g_host_symbols;
}
