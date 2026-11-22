/* Add-on registry: add, get by pointer and by text, duplicate and full table refused, count and
 * name_at. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

static const char *const filler_names[BR_ADDON_MAX] = {
    "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
    "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",
};

int main(void)
{
    br_engine *e = br_engine_create("test_addon", NULL);
    BR_CHECK(e != NULL);
    if (e == NULL) {
        return br_test_finish();
    }
    int net_state = 1;
    int tls_state = 2;

    BR_CHECK(br_addon_count(e) == 0);
    BR_CHECK(br_addon_get(e, "net") == NULL);
    BR_CHECK(br_addon_add(e, "net", &net_state));
    BR_CHECK(br_addon_add(e, "tls", &tls_state));
    BR_CHECK(br_addon_get(e, "net") == &net_state);
    BR_CHECK(br_addon_get(e, "tls") == &tls_state);

    /* the same text at a different address is still found */
    char name[8];
    SDL_strlcpy(name, "tls", sizeof(name));
    BR_CHECK(br_addon_get(e, name) == &tls_state);

    /* a taken name is refused and keeps the first state */
    BR_CHECK(!br_addon_add(e, "net", &tls_state));
    BR_CHECK(!br_addon_add(e, name, &net_state));
    BR_CHECK(br_addon_get(e, "net") == &net_state);

    BR_CHECK(br_addon_get(e, "http") == NULL);
    BR_CHECK(br_addon_get(e, NULL) == NULL);
    BR_CHECK(!br_addon_add(e, NULL, &net_state));

    BR_CHECK(br_addon_count(e) == 2);
    BR_CHECK(SDL_strcmp(br_addon_name_at(e, 0), "net") == 0);
    BR_CHECK(SDL_strcmp(br_addon_name_at(e, 1), "tls") == 0);
    BR_CHECK(br_addon_name_at(e, 2) == NULL);

    /* fill the table: BR_ADDON_MAX succeed in all, the next one fails */
    BR_CHECK(br_addon_add(e, filler_names[2], NULL));
    BR_CHECK(!br_addon_add(e, "f2", NULL)); /* taken even though its state is NULL */
    for (uint32_t i = 3; i < BR_ADDON_MAX; i++) {
        BR_CHECK(br_addon_add(e, filler_names[i], NULL));
    }
    BR_CHECK(br_addon_count(e) == BR_ADDON_MAX);
    BR_CHECK(!br_addon_add(e, "one_too_many", &net_state));
    BR_CHECK(br_addon_count(e) == BR_ADDON_MAX);
    BR_CHECK(br_addon_get(e, "one_too_many") == NULL);
    BR_CHECK(SDL_strcmp(br_addon_name_at(e, BR_ADDON_MAX - 1), "f15") == 0);
    BR_CHECK(br_addon_name_at(e, BR_ADDON_MAX) == NULL);

    br_engine_destroy(e);
    return br_test_finish();
}
