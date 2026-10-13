#ifndef LOG_H
#define LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Sends stdout / stderr through a pipe to a logger thread that saves
 * everything to `path` and mirrors it to the console without ever making
 * the writer wait: if the console stalls (for example while text is being
 * selected in it), console output is skipped rather than freezing the game.
 * Also turns off the console's QuickEdit mode. */
void log_init(const char *path);

/* Waits briefly until everything written so far is in the log file.
 * Call before the process exits or after a crash. */
void log_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* LOG_H */
