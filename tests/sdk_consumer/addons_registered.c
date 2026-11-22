/* Links braty::websocket alone and checks that br_websocket_register registers the add-ons it
 * depends on (http, tls, net), built from the SDK. No network: exit 0 = all four registered. */
#include "braty/braty.h"
#include "braty/br_websocket.h"

#include <SDL3/SDL.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    br_engine *e = br_engine_create("addons_registered", NULL);
    if (e == NULL || !br_websocket_register(e)) {
        return 1;
    }
    const char *names[] = { "net", "tls", "http", "websocket" };
    int missing = 0;
    for (int i = 0; i < 4; i++) {
        if (br_addon_get(e, names[i]) == NULL) {
            SDL_Log("add-on '%s' not registered", names[i]);
            missing++;
        }
    }
    SDL_Log("%u add-ons registered", (unsigned)br_addon_count(e));
    br_engine_destroy(e);
    return missing == 0 ? 0 : 1;
}
