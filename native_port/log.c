#include "log.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define PIPE_BYTES     (1 << 20)
#define CONSOLE_RING   (256 * 1024)

static HANDLE g_pipe_read = NULL;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static HANDLE g_console = INVALID_HANDLE_VALUE;
static HANDLE g_console_event = NULL;
static CRITICAL_SECTION g_ring_lock;
static char g_ring[CONSOLE_RING];
static size_t g_ring_read = 0, g_ring_used = 0;
static int g_ring_dropped = 0;
static volatile LONG g_logger_busy = 0;

static void ring_push(const char *data, size_t n) {
    EnterCriticalSection(&g_ring_lock);
    if (g_ring_used + n > CONSOLE_RING) {
        g_ring_dropped = 1; /* console can't keep up: skip, never block */
    } else {
        for (size_t i = 0; i < n; i++) {
            g_ring[(g_ring_read + g_ring_used + i) % CONSOLE_RING] = data[i];
        }
        g_ring_used += n;
    }
    LeaveCriticalSection(&g_ring_lock);
    SetEvent(g_console_event);
}

static DWORD WINAPI logger_thread(LPVOID unused) {
    char buf[8192];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(g_pipe_read, buf, sizeof(buf), &n, NULL) || n == 0) {
            Sleep(10);
            continue;
        }
        InterlockedExchange(&g_logger_busy, 1);
        if (g_file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(g_file, buf, n, &written, NULL);
        }
        if (g_console != INVALID_HANDLE_VALUE) ring_push(buf, n);
        InterlockedExchange(&g_logger_busy, 0);
    }
    return 0;
}

static DWORD WINAPI console_thread(LPVOID unused) {
    char chunk[8192];
    for (;;) {
        WaitForSingleObject(g_console_event, INFINITE);
        for (;;) {
            size_t n = 0;
            int dropped;
            EnterCriticalSection(&g_ring_lock);
            dropped = g_ring_dropped;
            g_ring_dropped = 0;
            while (n < sizeof(chunk) && g_ring_used > 0) {
                chunk[n++] = g_ring[g_ring_read];
                g_ring_read = (g_ring_read + 1) % CONSOLE_RING;
                g_ring_used--;
            }
            LeaveCriticalSection(&g_ring_lock);
            DWORD written;
            if (dropped) {
                static const char note[] = "\r\n[log] ... console output skipped (see game_log.txt) ...\r\n";
                WriteFile(g_console, note, sizeof(note) - 1, &written, NULL);
            }
            if (n == 0) break;
            WriteFile(g_console, chunk, (DWORD)n, &written, NULL); /* may block; only this thread waits */
        }
    }
    return 0;
}

void log_init(const char *path) {
    /* QuickEdit: clicking in the console starts a text selection that
     * pauses every write to it until a key is pressed */
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        SetConsoleMode(in, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    }

    HANDLE pipe_write = NULL;
    if (!CreatePipe(&g_pipe_read, &pipe_write, NULL, PIPE_BYTES)) return;

    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    InitializeCriticalSection(&g_ring_lock);
    g_console_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    int fd = _open_osfhandle((intptr_t)pipe_write, _O_TEXT);
    if (fd < 0) return;
    fflush(stdout);
    fflush(stderr);
    _dup2(fd, 1);
    _dup2(fd, 2);
    SetStdHandle(STD_OUTPUT_HANDLE, pipe_write);
    SetStdHandle(STD_ERROR_HANDLE, pipe_write);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    CloseHandle(CreateThread(NULL, 0, logger_thread, NULL, 0, NULL));
    if (g_console != INVALID_HANDLE_VALUE && g_console != NULL) {
        CloseHandle(CreateThread(NULL, 0, console_thread, NULL, 0, NULL));
    } else {
        g_console = INVALID_HANDLE_VALUE;
    }
}

void log_flush(void) {
    fflush(stdout);
    fflush(stderr);
    if (!g_pipe_read) return;
    for (int i = 0; i < 100; i++) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe_read, NULL, 0, NULL, &avail, NULL)) break;
        if (avail == 0 && !g_logger_busy) break;
        Sleep(10);
    }
    if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
}
