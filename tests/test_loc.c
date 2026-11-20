/* test_loc - string tables, locale chain and fallbacks, placeholders, plural forms, missing keys,
 * text nodes bound by "text_key", play-test paths, game.json "locale", hostile tables, and a
 * translation longer than any buffer (headless; tables are written next to the executable). */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_log.h>
#include <string.h>

static int g_missing_logs;
static SDL_LogOutputFunction g_prev_log;
static void *g_prev_log_user;

static void count_log(void *user, int category, SDL_LogPriority priority, const char *message)
{
    (void)user;
    if (strstr(message, "no text for 'nope'") != NULL) {
        g_missing_logs++;
    }
    g_prev_log(g_prev_log_user, category, priority, message);
}

static void write_table(br_engine *e, const char *path, const char *json)
{
    BR_CHECK(br_file_write_all(e, path, json, strlen(json)));
}

static char *g_long_text; /* 200 000 bytes of UTF-8 (multi-byte characters) */

static void write_tables(br_engine *e)
{
    write_table(e, "strings/strings.en.json",
                "{ \"menu.play\": \"Play\", \"menu.quit\": \"Quit\", \"only.en\": \"English only\","
                "  \"hud.crates\": { \"one\": \"{count} crate\", \"other\": \"{count} crates\", \"zero\": \"No crates\" },"
                "  \"greet\": \"Hello, {name}! {unknown} stays.\", \"menu.title\": \"<b>Crate</b> Rush\","
                "  \"bad.number\": 5, \"bad.plural\": { \"few\": \"x\" }, \"bad.form\": { \"one\": 1 } }");
    write_table(e, "strings/strings.de.json",
                "{ \"menu.play\": \"Spielen\", \"only.de\": \"Nur Deutsch\","
                "  \"hud.crates\": { \"one\": \"{count} Kiste\", \"other\": \"{count} Kisten\" } }");
    write_table(e, "strings/strings.de-AT.json", "{ \"menu.play\": \"Spün\", \"only.at\": \"Servus\" }");
    write_table(e, "strings/strings.broken.json", "[1, 2, 3]");
    write_table(e, "strings/strings.zz.json", "{ \"menu.play\": \"ZZ\" }");
    /* a German sign far beyond the 63-byte component strings */
    size_t len = 200000;
    g_long_text = SDL_malloc(len + 1);
    for (size_t i = 0; i + 2 <= len; i += 2) {
        g_long_text[i] = (char)0xC3; /* "ü" */
        g_long_text[i + 1] = (char)0xBC;
    }
    g_long_text[len] = '\0';
    size_t size = len + 64;
    char *json = SDL_malloc(size);
    SDL_snprintf(json, size, "{ \"sign\": \"%s\" }", g_long_text);
    write_table(e, "strings/strings.long.json", json);
    SDL_free(json);
    /* the machine's own languages get tables too: setup must still pick the default */
    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    for (int i = 0; locales != NULL && i < count; i++) {
        char path[128];
        SDL_snprintf(path, sizeof(path), "strings/strings.%s.json", locales[i]->language);
        if (strcmp(locales[i]->language, "en") != 0 && strcmp(locales[i]->language, "de") != 0) {
            write_table(e, path, "{ \"menu.play\": \"SYSTEM\" }");
        }
    }
    SDL_free(locales);
}

static void test_setup_and_chain(br_engine *e)
{
    const char *fallbacks[] = { "en" };
    BR_CHECK(!br_loc_setup(e, "strings", "missing", fallbacks, 1)); /* the default needs a table */
    BR_CHECK(!br_loc_setup(e, "strings", "../en", NULL, 0));
    /* headless: the default, whatever the system prefers */
    BR_CHECK(br_loc_setup(e, "strings", "zz", fallbacks, 1));
    BR_CHECK(strcmp(br_loc_locale(e), "zz") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "menu.play"), "ZZ") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "menu.quit"), "Quit") == 0); /* from the fallback */

    BR_CHECK(br_loc_setup(e, "strings", "en", NULL, 0));
    BR_CHECK(strcmp(br_loc_locale(e), "en") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "menu.play"), "Play") == 0);
    BR_CHECK(br_loc_set_locale(e, "de-AT")); /* de-AT, then de, then the default en */
    BR_CHECK(strcmp(br_loc_locale(e), "de-AT") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "menu.play"), "Spün") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "only.de"), "Nur Deutsch") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "only.en"), "English only") == 0);
    BR_CHECK(br_loc_has(e, "only.at") && !br_loc_has(e, "nope"));
    BR_CHECK(br_loc_set_locale(e, "de_CH")); /* only the language has a table */
    BR_CHECK(strcmp(br_loc_get(e, "menu.play"), "Spielen") == 0 && !br_loc_has(e, "only.at"));

    /* refused: no table, a broken table, names that are not locale names; the locale stays */
    BR_CHECK(!br_loc_set_locale(e, "fr"));
    BR_CHECK(!br_loc_set_locale(e, "broken"));
    BR_CHECK(!br_loc_set_locale(e, "../strings/strings.en"));
    BR_CHECK(!br_loc_set_locale(e, ""));
    BR_CHECK(!br_loc_set_locale(e, "a-very-long-locale-name-that-is-silly"));
    BR_CHECK(strcmp(br_loc_locale(e), "de_CH") == 0 && strcmp(br_loc_get(e, "menu.play"), "Spielen") == 0);

    /* bad entries were skipped (logged), the rest of the table loaded */
    BR_CHECK(br_loc_set_locale(e, "en"));
    BR_CHECK(!br_loc_has(e, "bad.number") && !br_loc_has(e, "bad.plural") && !br_loc_has(e, "bad.form"));
    /* "strings.json" is no table: the plain json loader keeps it */
    write_table(e, "strings.json", "{ \"a\": 1 }");
    void *plain = br_assets_load(e, "strings.json");
    BR_CHECK(plain != NULL && strcmp(br_assets_type(e, "strings.json"), "json") == 0);
    br_assets_unload(e, "strings.json");
}

static void test_missing_and_format(br_engine *e)
{
    BR_CHECK(br_loc_set_locale(e, "en"));
    g_missing_logs = 0;
    BR_CHECK(strcmp(br_loc_get(e, "nope"), "nope") == 0); /* the key is shown */
    BR_CHECK(strcmp(br_loc_get(e, "nope"), "nope") == 0);
    BR_CHECK(strcmp(br_loc_format(e, "nope", NULL, 0), "nope") == 0);
    BR_CHECK(g_missing_logs == 1); /* logged once */
    BR_CHECK(br_loc_set_locale(e, "de"));
    br_loc_get(e, "nope");
    BR_CHECK(g_missing_logs == 2); /* once per locale */
    BR_CHECK(br_loc_set_locale(e, "en"));

    br_loc_arg name[] = { { "name", "Ann" } };
    BR_CHECK(strcmp(br_loc_format(e, "greet", name, 1), "Hello, Ann! {unknown} stays.") == 0);
    br_loc_arg zero[] = { { "count", "0" } };
    br_loc_arg one[] = { { "count", "1" } };
    br_loc_arg many[] = { { "count", "12" } };
    BR_CHECK(strcmp(br_loc_format(e, "hud.crates", zero, 1), "No crates") == 0);
    BR_CHECK(strcmp(br_loc_format(e, "hud.crates", one, 1), "1 crate") == 0);
    BR_CHECK(strcmp(br_loc_format(e, "hud.crates", many, 1), "12 crates") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "hud.crates"), "{count} crates") == 0); /* get: the other form */
    BR_CHECK(br_loc_set_locale(e, "de"));
    BR_CHECK(strcmp(br_loc_format(e, "hud.crates", zero, 1), "0 Kisten") == 0); /* no zero form: other */
    BR_CHECK(strcmp(br_loc_format(e, "hud.crates", one, 1), "1 Kiste") == 0);
    BR_CHECK(br_loc_set_locale(e, "en"));
    BR_CHECK(strcmp(br_loc_format(e, "greet", NULL, 0), "Hello, {name}! {unknown} stays.") == 0);
}

/* Review focus: a translation longer than any buffer shows whole, never truncated, no crash. */
static void test_long_translation(br_engine *e)
{
    const char *fallbacks[] = { "en" };
    BR_CHECK(br_loc_setup(e, "strings", "en", fallbacks, 1));
    BR_CHECK(br_loc_set_locale(e, "long"));
    BR_CHECK(strcmp(br_loc_get(e, "sign"), g_long_text) == 0);
    size_t big_len = 1000000;
    char *big = SDL_malloc(big_len + 1);
    SDL_memset(big, 'x', big_len);
    big[big_len] = '\0';
    br_loc_arg arg[] = { { "name", big } };
    const char *formatted = br_loc_format(e, "greet", arg, 1);
    BR_CHECK(strlen(formatted) == big_len + strlen("Hello, ! {unknown} stays."));
    br_node t = br_text_create(e, "", NULL);
    br_node_add_child(e, br_stage_root(e), t);
    br_loc_bind_text(e, t, "sign");
    BR_CHECK(strcmp(br_text_text(e, t), g_long_text) == 0);
    br_engine_step(e, 1.0f / 60.0f); /* lays it out */
    BR_CHECK(br_text_char_count(e, t) == 100000);
    br_node_destroy(e, t);
    br_engine_step(e, 1.0f / 60.0f);
    SDL_free(big);
    BR_CHECK(br_loc_set_locale(e, "en"));
}

static void test_bound_nodes(br_engine *e)
{
    BR_CHECK(br_loc_set_locale(e, "en"));
    const char *json = "{ \"children\": ["
                       "  { \"type\": \"text\", \"label\": \"play\", \"text\": \"placeholder\", \"text_key\": \"menu.play\" },"
                       "  { \"type\": \"rich_text\", \"label\": \"title\", \"text_key\": \"menu.title\" },"
                       "  { \"type\": \"text\", \"label\": \"gone\", \"text_key\": \"menu.quit\" } ] }";
    br_node root = br_scene_file_load_json(e, json, strlen(json), "loc.scene.json", br_stage_root(e));
    br_node play = br_node_find_by_label(e, root, "play", true);
    br_node title = br_node_find_by_label(e, root, "title", true);
    br_node gone = br_node_find_by_label(e, root, "gone", true);
    BR_CHECK(strcmp(br_text_text(e, play), "Play") == 0);
    BR_CHECK(strcmp(br_loc_text_key(e, play), "menu.play") == 0);
    BR_CHECK(strcmp(br_text_text(e, title), "<b>Crate</b> Rush") == 0);
    br_engine_step(e, 1.0f / 60.0f);
    BR_CHECK(strcmp(br_rich_text_plain(e, title), "Crate Rush") == 0);
    br_node_destroy(e, gone);
    br_engine_step(e, 1.0f / 60.0f); /* freed: its binding goes with it */
    BR_CHECK(br_loc_set_locale(e, "de-AT"));
    BR_CHECK(strcmp(br_text_text(e, play), "Spün") == 0);
    BR_CHECK(strcmp(br_text_text(e, title), "<b>Crate</b> Rush") == 0); /* fallback en */
    /* a reused node slot does not inherit the old binding */
    br_node fresh = br_text_create(e, "mine", NULL);
    br_node_add_child(e, br_stage_root(e), fresh);
    BR_CHECK(strcmp(br_loc_text_key(e, fresh), "") == 0);
    BR_CHECK(br_loc_set_locale(e, "en"));
    BR_CHECK(strcmp(br_text_text(e, fresh), "mine") == 0);
    /* saving keeps the key, so a reload follows the locale too */
    cJSON *saved = br_scene_file_to_json(e, root);
    char *printed = cJSON_PrintUnformatted(saved);
    BR_CHECK(printed != NULL && strstr(printed, "\"text_key\":\"menu.play\"") != NULL);
    cJSON_free(printed);
    cJSON_Delete(saved);
    br_loc_bind_text(e, play, NULL); /* unbound: keeps its text, no longer follows */
    BR_CHECK(br_loc_set_locale(e, "de"));
    BR_CHECK(strcmp(br_text_text(e, play), "Play") == 0);
    BR_CHECK(br_loc_set_locale(e, "en"));
    br_node_destroy(e, fresh);
    br_node_destroy(e, root);
    br_engine_step(e, 1.0f / 60.0f);
}

static void test_playtest(br_engine *e)
{
    BR_CHECK(br_loc_set_locale(e, "en"));
    cJSON *v = br_playtest_get(e, "loc:menu.play");
    BR_CHECK(cJSON_IsString(v) && strcmp(v->valuestring, "Play") == 0);
    cJSON_Delete(v);
    cJSON *de = cJSON_CreateString("de");
    BR_CHECK(br_playtest_set(e, "engine:locale", de));
    cJSON_Delete(de);
    v = br_playtest_get(e, "engine:locale");
    BR_CHECK(cJSON_IsString(v) && strcmp(v->valuestring, "de") == 0);
    cJSON_Delete(v);
    v = br_playtest_get(e, "loc:menu.play");
    BR_CHECK(cJSON_IsString(v) && strcmp(v->valuestring, "Spielen") == 0);
    cJSON_Delete(v);
    cJSON *fr = cJSON_CreateString("fr");
    BR_CHECK(!br_playtest_set(e, "engine:locale", fr));
    cJSON_Delete(fr);
    BR_CHECK(br_loc_set_locale(e, "en"));
}

static void test_game_config(br_engine *e)
{
    const char *json = "{ \"locale\": { \"default\": \"de\", \"fallback\": [\"en\"], \"dir\": \"strings\" } }";
    br_game_config config;
    BR_CHECK(br_game_config_parse(json, strlen(json), "game.json", &config));
    br_game_config_apply(e, &config);
    BR_CHECK(strcmp(br_loc_locale(e), "de") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "menu.quit"), "Quit") == 0);
    br_game_config_free(&config);
}

int main(void)
{
    char root[1024];
    SDL_snprintf(root, sizeof(root), "%sloc_test/strings", SDL_GetBasePath());
    SDL_CreateDirectory(root);
    SDL_GetLogOutputFunction(&g_prev_log, &g_prev_log_user);
    SDL_SetLogOutputFunction(count_log, NULL);
    br_engine *e = br_engine_create("test_loc", "loc_test"); /* relative to the exe */
    if (e == NULL) {
        BR_CHECK(false);
        return br_test_finish();
    }
    write_tables(e);
    BR_CHECK(strcmp(br_loc_locale(e), "") == 0);
    BR_CHECK(strcmp(br_loc_get(e, "menu.play"), "menu.play") == 0); /* before any setup: the key */
    test_setup_and_chain(e);
    test_missing_and_format(e);
    test_long_translation(e);
    test_bound_nodes(e);
    test_playtest(e);
    test_game_config(e);
    br_engine_destroy(e);
    SDL_free(g_long_text);
    SDL_SetLogOutputFunction(g_prev_log, g_prev_log_user);
    return br_test_finish();
}
