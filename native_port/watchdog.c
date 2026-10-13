#include "watchdog.h"
#include "shims.h"
#include "android_ndk.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Hang detector
 *
 * A background thread watches the presented-frame counter. When it stops
 * advancing for HANG_SECONDS while the window is focused, each registered
 * game thread is suspended in turn and its registers plus a scan of its
 * stack for return addresses into libminigore2.so are written to
 * hang_report.txt. Addresses are symbolized against the ELF dynamic symbol
 * table (mangled C++ names; pipe through c++filt to demangle). */

#define HANG_SECONDS       10   /* during play */
#define LOAD_HANG_SECONDS  60   /* first frames: shader compilation and loading */
#define LOADING_FRAMES     300  /* ~5 s of presented frames */
#define MAX_WD_THREADS     64
#define STACK_SCAN_BYTES   (64 * 1024)
#define MAX_STACK_FRAMES   24

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} WdElfSym;

#define WD_STT_FUNC 2

typedef struct {
    HANDLE handle;
    DWORD tid;
    char name[16];
} WdThread;

static const elf_module_t *g_mod = NULL;
static WdThread g_wd_threads[MAX_WD_THREADS];
static int g_wd_thread_count = 0;
static CRITICAL_SECTION g_wd_lock;
static volatile LONG g_wd_lock_ready = 0;
static volatile LONG g_frames = 0;
static volatile LONG g_focused = 1;

static void wd_lock_init(void) {
    static volatile LONG initialising = 0;
    if (g_wd_lock_ready) return;
    if (InterlockedCompareExchange(&initialising, 1, 0) == 0) {
        InitializeCriticalSection(&g_wd_lock);
        InterlockedExchange(&g_wd_lock_ready, 1);
    } else {
        while (!g_wd_lock_ready) Sleep(0);
    }
}

void watchdog_register_thread(HANDLE thread, const char *name) {
    HANDLE dup = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &dup,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                         FALSE, 0)) {
        return;
    }
    wd_lock_init();
    EnterCriticalSection(&g_wd_lock);
    /* Reuse slots of threads that have exited */
    int slot = -1;
    for (int i = 0; i < g_wd_thread_count; i++) {
        if (WaitForSingleObject(g_wd_threads[i].handle, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_wd_threads[i].handle);
            slot = i;
            break;
        }
    }
    if (slot < 0 && g_wd_thread_count < MAX_WD_THREADS) slot = g_wd_thread_count++;
    if (slot >= 0) {
        g_wd_threads[slot].handle = dup;
        g_wd_threads[slot].tid = GetThreadId(dup);
        snprintf(g_wd_threads[slot].name, sizeof(g_wd_threads[slot].name), "%s", name ? name : "thread");
    } else {
        CloseHandle(dup);
    }
    LeaveCriticalSection(&g_wd_lock);
}

void watchdog_frame(void) {
    InterlockedIncrement(&g_frames);
}

void watchdog_set_focused(int focused) {
    InterlockedExchange(&g_focused, focused ? 1 : 0);
}

/* "symbol+0xoff" for addresses inside the game library, else module+offset */
void watchdog_describe_address(uint32_t addr, char *out, size_t out_size);

static void describe_address(uint32_t addr, char *out, size_t out_size) {
    uintptr_t base = g_mod ? (uintptr_t)g_mod->base : 0;
    if (g_mod && addr >= base && addr < base + g_mod->size) {
        uint32_t rel = addr - (uint32_t)base;
        const WdElfSym *syms = (const WdElfSym *)g_mod->dynsym;
        const WdElfSym *best = NULL;
        for (size_t i = 0; i < g_mod->num_dynsym; i++) {
            const WdElfSym *s = &syms[i];
            if ((s->st_info & 0xF) != WD_STT_FUNC || s->st_shndx == 0) continue;
            if (s->st_value <= rel && (!best || s->st_value > best->st_value)) best = s;
        }
        if (best) {
            snprintf(out, out_size, "libminigore2.so!%s+0x%x", g_mod->dynstr + best->st_name, rel - best->st_value);
        } else {
            snprintf(out, out_size, "libminigore2.so+0x%x", rel);
        }
        return;
    }

    HMODULE hm = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &hm) &&
        GetModuleFileNameA(hm, path, sizeof(path))) {
        const char *slash = strrchr(path, '\\');
        snprintf(out, out_size, "%s+0x%x", slash ? slash + 1 : path, addr - (uint32_t)(uintptr_t)hm);
        return;
    }
    snprintf(out, out_size, "0x%08x", addr);
}

static int is_code_address(uint32_t addr) {
    if (g_mod) {
        uintptr_t base = (uintptr_t)g_mod->base;
        if (addr >= base && addr < base + g_mod->size) return 1;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* Snapshot taken while the thread is suspended. Nothing in the capture
 * path may take a lock (no stdio, heap or loader calls): the suspended
 * thread could be holding it, which would deadlock the watchdog. */
typedef struct {
    int ok;
    CONTEXT ctx;
    uint32_t stack[STACK_SCAN_BYTES / 4];
    uint32_t stack_words;
} WdSnapshot;

static WdSnapshot g_snap;

static void capture_thread(const WdThread *t, WdSnapshot *snap) {
    snap->ok = 0;
    snap->stack_words = 0;
    if (SuspendThread(t->handle) == (DWORD)-1) return;
    memset(&snap->ctx, 0, sizeof(snap->ctx));
    snap->ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(t->handle, &snap->ctx)) {
        snap->ok = 1;
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t esp = snap->ctx.Esp & ~3u;
        if (VirtualQuery((LPCVOID)esp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
            uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (end > esp + STACK_SCAN_BYTES) end = esp + STACK_SCAN_BYTES;
            snap->stack_words = (uint32_t)((end - esp) / 4);
            memcpy(snap->stack, (const void *)esp, snap->stack_words * 4);
        }
    }
    ResumeThread(t->handle);
}

static void print_snapshot(FILE *out, const WdThread *t, const WdSnapshot *snap) {
    char desc[512];
    if (!snap->ok) {
        fprintf(out, "\n-- thread %lu (%s): could not sample\n", t->tid, t->name);
        return;
    }
    const CONTEXT *ctx = &snap->ctx;
    fprintf(out, "\n-- thread %lu (%s)\n", t->tid, t->name);
    describe_address(ctx->Eip, desc, sizeof(desc));
    fprintf(out, "   EIP %08lx  %s\n", ctx->Eip, desc);
    fprintf(out, "   EAX %08lx EBX %08lx ECX %08lx EDX %08lx ESI %08lx EDI %08lx EBP %08lx ESP %08lx\n",
            ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);

    /* Heuristic stack walk: any stack word pointing into executable code is
     * a likely return address. */
    int frames = 0;
    for (uint32_t i = 0; i < snap->stack_words && frames < MAX_STACK_FRAMES; i++) {
        uint32_t v = snap->stack[i];
        if (v < 0x10000 || !is_code_address(v)) continue;
        describe_address(v, desc, sizeof(desc));
        fprintf(out, "   [esp+%04x] %08x  %s\n", i * 4, v, desc);
        frames++;
    }
}

static void write_report(void) {
    FILE *out = fopen("hang_report.txt", "w");
    FILE *outs[2] = { stdout, out };

    long mutexes = 0, conds = 0;
    shims_get_sync_stats(&mutexes, &conds);
    time_t now = time(NULL);

    for (int o = 0; o < 2; o++) {
        if (!outs[o]) continue;
        fprintf(outs[o], "\n================ HANG DETECTED ================\n");
        fprintf(outs[o], "No frame presented for %d s (frame %ld) at %s",
                g_frames < LOADING_FRAMES ? LOAD_HANG_SECONDS : HANG_SECONDS, g_frames, ctime(&now));
        fprintf(outs[o], "Host sync objects: %ld mutexes, %ld condition variables\n", mutexes, conds);
        fprintf(outs[o], "Input queue depth: %d\n", android_app_input_queue_depth());
        if (g_mod) fprintf(outs[o], "libminigore2.so loaded at %p\n", g_mod->base);
    }

    /* g_wd_lock is only taken by thread registration, which never happens
     * while holding a lock the game threads could be suspended inside. */
    EnterCriticalSection(&g_wd_lock);
    for (int i = 0; i < g_wd_thread_count; i++) {
        if (WaitForSingleObject(g_wd_threads[i].handle, 0) == WAIT_OBJECT_0) continue;
        capture_thread(&g_wd_threads[i], &g_snap);
        for (int o = 0; o < 2; o++) {
            if (outs[o]) print_snapshot(outs[o], &g_wd_threads[i], &g_snap);
        }
    }
    LeaveCriticalSection(&g_wd_lock);

    printf("\n[Watchdog] Report written to hang_report.txt (full log: game_log.txt)\n");
    fflush(stdout);
    if (out) fclose(out);
}

static DWORD WINAPI watchdog_proc(LPVOID unused) {
    LONG last_frames = g_frames;
    int stalled_seconds = 0;
    int reported = 0;

    for (;;) {
        Sleep(1000);
        LONG frames = g_frames;
        /* Ignore stalls before the first frame (loading) and while unfocused */
        if (frames != last_frames || frames == 0 || !g_focused) {
            last_frames = frames;
            stalled_seconds = 0;
            reported = 0;
            continue;
        }
        int limit = frames < LOADING_FRAMES ? LOAD_HANG_SECONDS : HANG_SECONDS;
        if (++stalled_seconds >= limit && !reported) {
            write_report();
            reported = 1; /* once per stall */
        }
    }
    return 0;
}

void watchdog_start(const elf_module_t *mod) {
    g_mod = mod;
    wd_lock_init();
    HANDLE h = CreateThread(NULL, 0, watchdog_proc, NULL, 0, NULL);
    if (h) CloseHandle(h);
    printf("[Watchdog] Hang detection enabled (%d s in play, %d s while starting up)\n",
           HANG_SECONDS, LOAD_HANG_SECONDS);
    fflush(stdout);
}

void watchdog_describe_address(uint32_t addr, char *out, size_t out_size) {
    describe_address(addr, out, out_size);
}
