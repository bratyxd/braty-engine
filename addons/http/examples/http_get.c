/* http_get <url> [max_body_bytes]: fetches a URL with the http add-on and prints the status, the
 * headers and the body size (headless: no window). Needs the internet for public URLs, so CTest
 * does not run it. */
#include "braty/braty.h"
#include "braty/br_http.h"

#include <SDL3/SDL.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        SDL_Log("usage: http_get <url> [max_body_bytes]");
        return 2;
    }
    br_engine *e = br_engine_create("http_get", NULL);
    if (e == NULL || !br_http_register(e)) {
        return 1;
    }
    br_http_desc desc;
    SDL_zero(desc);
    desc.url = argv[1];
    desc.max_body = argc > 2 ? (size_t)SDL_strtoull(argv[2], NULL, 10) : 0;
    uint64_t start = SDL_GetTicksNS();
    br_http_request r = br_http_send(e, &desc);
    float shown = -1.0f;
    while (br_http_request_state(e, r) == BR_HTTP_PENDING) {
        br_engine_step(e, 1.0f / 60.0f);
        float progress = br_http_progress(e, r);
        if (progress >= shown + 0.25f) {
            SDL_Log("... %.0f%%", (double)progress * 100.0);
            shown = progress;
        }
        SDL_Delay(1);
    }
    double seconds = (double)(SDL_GetTicksNS() - start) / 1e9;
    int code = 0;
    if (br_http_request_state(e, r) == BR_HTTP_DONE) {
        size_t size = 0;
        br_http_body(e, r, &size);
        SDL_Log("status %d, body %llu bytes, %.2f s", br_http_status(e, r), (unsigned long long)size, seconds);
        const char *names[] = { "Content-Type", "Content-Length", "Transfer-Encoding", "Location", "Server", "Date" };
        for (size_t i = 0; i < BR_ARRAY_COUNT(names); i++) {
            const char *value = br_http_header(e, r, names[i]);
            if (value != NULL) {
                SDL_Log("  %s: %s", names[i], value);
            }
        }
    } else {
        SDL_Log("failed after %.2f s: %s", seconds, br_http_error(e, r));
        code = 1;
    }
    br_http_release(e, r);
    br_engine_destroy(e);
    return code;
}
