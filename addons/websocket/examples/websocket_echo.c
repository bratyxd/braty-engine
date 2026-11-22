/* websocket_echo <url> <text>: connects with the websocket add-on, sends text, prints every
 * message until its echo comes back, then closes cleanly and prints the close code (headless:
 * no window). Needs the internet for public servers, so CTest does not run it. */
#include "braty/braty.h"
#include "braty/br_websocket.h"

#include <SDL3/SDL.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        SDL_Log("usage: websocket_echo <ws:// or wss:// url> <text>");
        return 2;
    }
    br_engine *e = br_engine_create("websocket_echo", NULL);
    if (e == NULL || !br_websocket_register(e)) {
        return 1;
    }
    uint64_t start = SDL_GetTicksNS();
    uint64_t deadline = start + 30000000000ull;
    br_websocket ws = br_websocket_open(e, argv[1], NULL, NULL);
    bool sent = false;
    bool echoed = false;
    br_websocket_state state = br_websocket_socket_state(e, ws);
    while ((state == BR_WEBSOCKET_CONNECTING || state == BR_WEBSOCKET_OPEN || state == BR_WEBSOCKET_CLOSING) &&
           SDL_GetTicksNS() < deadline) {
        br_engine_step(e, 1.0f / 60.0f);
        state = br_websocket_socket_state(e, ws);
        if (state == BR_WEBSOCKET_OPEN && !sent) {
            SDL_Log("open after %.2f s; sending \"%s\"", (double)(SDL_GetTicksNS() - start) / 1e9, argv[2]);
            sent = br_websocket_send_text(e, ws, argv[2]);
        }
        br_websocket_message m = { NULL, 0, false };
        while (br_websocket_receive(e, ws, &m)) {
            SDL_Log("received %s message, %llu bytes: %.200s", m.text ? "a text" : "a binary",
                    (unsigned long long)m.size, m.text ? (const char *)m.data : "");
            if (m.text && SDL_strcmp(m.data, argv[2]) == 0 && !echoed) {
                echoed = true;
                SDL_Log("echo received; closing");
                br_websocket_close(e, ws, 1000, "done");
            }
        }
        SDL_Delay(1);
    }
    const char *names[] = { "CLOSED", "CONNECTING", "OPEN", "CLOSING", "FAILED" };
    SDL_Log("%s after %.2f s, close code %u, error \"%s\"", names[state], (double)(SDL_GetTicksNS() - start) / 1e9,
            (unsigned)br_websocket_close_code(e, ws), br_websocket_error(e, ws));
    bool ok = echoed && state == BR_WEBSOCKET_CLOSED && br_websocket_close_code(e, ws) == 1000;
    br_websocket_release(e, ws);
    br_engine_destroy(e);
    return ok ? 0 : 1;
}
