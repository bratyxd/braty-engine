/* braty_main.h - include this in exactly one .c file of a game, which defines br_main().
 *
 *   #include "braty/braty_main.h"
 *
 *   static bool game_init(br_engine *e) { ... register systems ...; return true; }
 *
 *   br_app_desc br_main(int argc, char **argv)
 *   {
 *       br_app_desc desc = { 0 };
 *       desc.title = "My Game";
 *       desc.init = game_init;
 *       return desc;
 *   }
 *
 * It uses SDL's main callbacks (no blocking main loop), which every SDL platform supports.
 */
#ifndef BRATY_MAIN_H
#define BRATY_MAIN_H

#include "braty/braty.h"

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    br_app_desc desc = br_main(argc, argv);
    return br_app_init(appstate, &desc, argc, argv);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    return br_app_iterate(appstate);
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    return br_app_event(appstate, event);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    br_app_quit(appstate, result);
}

#endif
