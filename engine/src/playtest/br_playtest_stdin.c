/* --playtest-stdin: one JSON command per line on stdin, one JSON reply per line on stdout.
 *
 * THE ONE EXCEPTION to "SDL only, no C stdio" in the engine: SDL has no portable way to read
 * the process's standard input, so this file (and only this file) uses <stdio.h>. It is built
 * only with the CMake option BRATY_PLAYTEST_STDIN (on by default on desktop, off on mobile and
 * consoles, where the stub below reports that interactive mode is unavailable).
 *
 * Design: the read blocks, but only while the game is deliberately paused waiting for its next
 * command (the game advances only when told to), so there is no reader thread and no queue.
 * While blocked, SDL events are not pumped; the window may show as "not responding" until the
 * next command arrives, which does not affect frames or screenshots.
 */
#include "playtest/br_playtest_internal.h"

#ifdef BRATY_PLAYTEST_STDIN

#include <stdio.h>

#define BR_PLAYTEST_LINE_SIZE 65536

SDL_AppResult br_playtest_stdin_iterate(br_engine *e)
{
    char line[BR_PLAYTEST_LINE_SIZE]; /* desktop-only file: a 64 KB stack buffer is fine */
    if (fgets(line, sizeof(line), stdin) == NULL) {
        br_log_info("playtest: stdin closed, quitting");
        return e->playtest->failed ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    }
    size_t len = SDL_strlen(line);
    if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
        /* longer than the buffer: drop the rest so the line still gets exactly one reply */
        int c = fgetc(stdin);
        while (c != EOF && c != '\n') {
            c = fgetc(stdin);
        }
        fprintf(stdout, "{\"ok\":false,\"frame\":%llu,\"error\":\"command line longer than 64 KB\"}\n",
                (unsigned long long)e->ticker.frame);
        fflush(stdout);
        e->playtest->failed = true;
        return SDL_APP_CONTINUE;
    }
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return SDL_APP_CONTINUE; /* blank lines are ignored */
    }
    char *reply = br_playtest_command(e, line);
    fputs(reply != NULL ? reply : "{\"ok\":false,\"error\":\"out of memory\"}", stdout);
    fputc('\n', stdout);
    fflush(stdout);
    br_free(reply);
    if (e->quit_requested) {
        return e->exit_failure ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

#else

SDL_AppResult br_playtest_stdin_iterate(br_engine *e)
{
    (void)e;
    br_log_error("--playtest-stdin is not available in this build (CMake option BRATY_PLAYTEST_STDIN is off)");
    return SDL_APP_FAILURE;
}

#endif
