#include "log.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PIPE_BYTES     (1 << 20)
#define CONSOLE_RING   (256 * 1024)
#define MAX_LOG_BYTES  (50u * 1024u * 1024u)  /* stop growing game_log.txt here */
#define MAX_LINE       1024
#define RECENT_LINES   128                    /* distinct lines remembered for de-duplication */
#define REPEAT_WINDOW  10000                  /* ms: a line seen this recently counts as a repeat */
#define SUMMARY_EVERY  5000                   /* ms between "suppressed" summaries */

/* Flood control: the first occurrence of a line is logged; repeats within
 * REPEAT_WINDOW are counted and summarised (with the noisiest lines named)
 * so a runaway message can't fill the disk but is still identified. */
typedef struct {
    uint32_t hash;
    DWORD last_seen;
    unsigned suppressed;
    char text[120];
} RecentLine;

static RecentLine g_recent[RECENT_LINES];
static unsigned g_suppressed_total = 0;
static DWORD g_last_summary = 0;
static char g_line[MAX_LINE];
static size_t g_line_len = 0;
static unsigned long long g_file_bytes = 0;
static int g_file_full = 0;
static CRITICAL_SECTION g_emit_lock;  /* logger thread vs log_flush */

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

static void emit(const char *data, size_t n) {
    if (g_file != INVALID_HANDLE_VALUE && !g_file_full) {
        DWORD written;
        if (g_file_bytes + n > MAX_LOG_BYTES) {
            static const char note[] = "[log] game_log.txt reached its size limit; further output goes to the console only\r\n";
            WriteFile(g_file, note, sizeof(note) - 1, &written, NULL);
            g_file_full = 1;
        } else {
            WriteFile(g_file, data, (DWORD)n, &written, NULL);
            g_file_bytes += n;
        }
    }
    if (g_console != INVALID_HANDLE_VALUE) ring_push(data, n);
}

static uint32_t hash_line(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        /* digits ignored so "frame 101" and "frame 102" count as one line */
        if (s[i] >= '0' && s[i] <= '9') continue;
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    }
    return h;
}

static void summarise(DWORD now) {
    g_last_summary = now;
    if (!g_suppressed_total) return;
    char msg[600];
    int len = snprintf(msg, sizeof(msg), "[log] %u repeated lines suppressed in the last %d s; noisiest:\r\n",
                       g_suppressed_total, SUMMARY_EVERY / 1000);
    emit(msg, (size_t)len);
    for (int pick = 0; pick < 3; pick++) {
        RecentLine *top = NULL;
        for (int i = 0; i < RECENT_LINES; i++) {
            if (g_recent[i].suppressed && (!top || g_recent[i].suppressed > top->suppressed)) top = &g_recent[i];
        }
        if (!top) break;
        len = snprintf(msg, sizeof(msg), "[log]   x%u  %s\r\n", top->suppressed, top->text);
        emit(msg, (size_t)len);
        top->suppressed = 0;
    }
    for (int i = 0; i < RECENT_LINES; i++) g_recent[i].suppressed = 0;
    g_suppressed_total = 0;
}

static void process_line(const char *line, size_t n) {
    DWORD now = GetTickCount();
    uint32_t h = hash_line(line, n);
    RecentLine *slot = NULL, *oldest = &g_recent[0];
    for (int i = 0; i < RECENT_LINES; i++) {
        if (g_recent[i].hash == h && now - g_recent[i].last_seen < REPEAT_WINDOW) {
            slot = &g_recent[i];
            break;
        }
        if (g_recent[i].last_seen < oldest->last_seen) oldest = &g_recent[i];
    }
    if (slot) {
        slot->last_seen = now;
        slot->suppressed++;
        g_suppressed_total++;
    } else {
        oldest->hash = h;
        oldest->last_seen = now;
        oldest->suppressed = 0;
        size_t t = n < sizeof(oldest->text) - 1 ? n : sizeof(oldest->text) - 1;
        while (t > 0 && (line[t - 1] == '\n' || line[t - 1] == '\r')) t--;
        memcpy(oldest->text, line, t);
        oldest->text[t] = '\0';
        emit(line, n);
    }
    if (now - g_last_summary >= SUMMARY_EVERY) summarise(now);
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
        EnterCriticalSection(&g_emit_lock);
        for (DWORD i = 0; i < n; i++) {
            if (g_line_len < MAX_LINE) g_line[g_line_len++] = buf[i];
            if (buf[i] == '\n' || g_line_len == MAX_LINE) {
                process_line(g_line, g_line_len);
                g_line_len = 0;
            }
        }
        LeaveCriticalSection(&g_emit_lock);
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
    InitializeCriticalSection(&g_emit_lock);
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
    if (g_file != INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&g_emit_lock);
        if (g_line_len) { /* a last line without a newline */
            emit(g_line, g_line_len);
            g_line_len = 0;
        }
        summarise(GetTickCount());
        LeaveCriticalSection(&g_emit_lock);
        FlushFileBuffers(g_file);
    }
}
