/* test_data_formats - every JSON example in docs/DATA_FORMATS.md loads without a warning or an
 * error. Each example is preceded by a line "<!-- check: KIND [PATH] -->": KIND says which loader
 * reads it, PATH (optional) is where the example is also written under the test's asset root so
 * other examples can reference it (a prefab, a clip library). The files the examples name
 * (atlas.json, fonts, a model, textures, a sound) are copied there from the repository first. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>
#include <string.h>

#define MAX_EXAMPLES 64

typedef struct example {
    int  line;          /* line of the check marker in DATA_FORMATS.md */
    char kind[32];
    char path[128];     /* "" = not written to a file */
    char *text;         /* the JSON between the fences (owned) */
} example;

static int g_problems;  /* warnings and errors logged since the counter was reset */
static SDL_LogOutputFunction g_prev_log;
static void *g_prev_log_user;

static void count_log(void *user, int category, SDL_LogPriority priority, const char *message)
{
    (void)user;
    if (priority >= SDL_LOG_PRIORITY_WARN) {
        g_problems++;
    }
    g_prev_log(g_prev_log_user, category, priority, message);
}

/* Copies line [start, end) of text into out (NUL-terminated, truncated to size). */
static void copy_line(const char *start, const char *end, char *out, size_t size)
{
    size_t n = (size_t)(end - start);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(out, start, n);
    out[n] = '\0';
}

/* Splits the document into examples. Returns the count; *fences = every ```json fence seen. */
static int collect_examples(const char *doc, example *out, int max, int *fences)
{
    int count = 0;
    int line = 0;
    *fences = 0;
    const char *p = doc;
    const example *pending = NULL;
    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        if (end == NULL) {
            end = p + strlen(p);
        }
        line++;
        char text[256];
        copy_line(p, end, text, sizeof(text));
        size_t len = strlen(text);
        if (len > 0 && text[len - 1] == '\r') {
            text[len - 1] = '\0';
        }
        if (strncmp(text, "<!-- check: ", 12) == 0 && count < max) {
            example *x = &out[count];
            SDL_zerop(x);
            x->line = line;
            /* "KIND [PATH] -->" */
            char *rest = text + 12;
            char *close = strstr(rest, " -->");
            if (close != NULL) {
                *close = '\0';
            }
            char *space = strchr(rest, ' ');
            if (space != NULL) {
                *space = '\0';
                SDL_strlcpy(x->path, space + 1, sizeof(x->path));
            }
            SDL_strlcpy(x->kind, rest, sizeof(x->kind));
            pending = x;
            count++;
        } else if (strcmp(text, "```json") == 0) {
            (*fences)++;
            const char *body = *end == '\n' ? end + 1 : end;
            const char *stop = strstr(body, "\n```");
            if (pending != NULL && stop != NULL && pending->text == NULL) {
                size_t n = (size_t)(stop - body);
                out[count - 1].text = SDL_malloc(n + 1);
                memcpy(out[count - 1].text, body, n);
                out[count - 1].text[n] = '\0';
            }
            pending = NULL;
        }
        p = *end == '\n' ? end + 1 : end;
    }
    return count;
}

/* Writes data to root/path, creating the directories on the way (SDL_CreateDirectory makes
 * missing parents too). */
static bool write_file(const char *root, const char *path, const void *data, size_t size)
{
    char full[1024];
    SDL_snprintf(full, sizeof(full), "%s%s", root, path);
    char *slash = strrchr(full, '/');
    if (slash != NULL) {
        *slash = '\0';
        SDL_CreateDirectory(full);
        *slash = '/';
    }
    return SDL_SaveFile(full, data, size);
}

/* Copies repository file `from` (relative to the repository root) to root/to. */
static bool stage_file(const char *root, const char *from, const char *to)
{
    char src[1024];
    SDL_snprintf(src, sizeof(src), "%s../../%s", SDL_GetBasePath(), from);
    size_t size = 0;
    void *data = SDL_LoadFile(src, &size);
    if (data == NULL) {
        printf("cannot read %s\n", src);
        return false;
    }
    bool ok = write_file(root, to, data, size);
    SDL_free(data);
    return ok;
}

/* The smallest GIF: 1 x 1, one white pixel (header, screen, palette, image, LZW data, trailer). */
static const unsigned char k_gif[] = {
    'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0xFF, 0xFF, 0xFF, 0, 0, 0,
    0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0x02, 0x02, 0x44, 0x01, 0, 0x3B,
};

/* Puts every file the examples reference under root. */
static void stage_assets(const char *root)
{
    static const char *const files[][2] = {
        { "examples/data/atlas.json", "atlas.json" },
        { "examples/data/atlas.png", "atlas.png" },
        { "examples/fonts/pixel5x7.fnt", "fonts/pixel.fnt" },
        { "examples/fonts/pixel5x7.png", "fonts/pixel5x7.png" },
        { "engine/assets/fonts/NotoSans-Regular.ttf", "fonts/Title.ttf" },
        { "examples/models/Fox/Fox.glb", "models/Fox/Fox.glb" },
        { "examples/audio/gong.ogg", "sounds/music.ogg" },
        { "examples/audio/beep.wav", "sounds/hit.wav" },
        { "examples/assets/checker.png", "a.png" },
        { "examples/assets/checker.png", "b.png" },
        { "examples/assets/checker.png", "rope.png" },
        { "examples/assets/checker.png", "bricks.png" },
        { "examples/assets/checker.png", "icon.png" },
        { "examples/assets/checker.png", "bg/stars.png" },
        { "examples/assets/checker.png", "fx/noise.png" },
    };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(files); i++) {
        BR_CHECK(stage_file(root, files[i][0], files[i][1]));
    }
    BR_CHECK(write_file(root, "fx/explosion.gif", k_gif, sizeof(k_gif)));
}

/* Loads every node object of a scene example (some examples list several objects in a row). */
static void load_nodes(br_engine *e, const char *text, const char *source)
{
    const char *p = text;
    int values = 0;
    for (;;) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *end = NULL;
        cJSON *node = cJSON_ParseWithOpts(p, &end, false);
        BR_CHECK(node != NULL);
        if (node == NULL) {
            printf("%s: not JSON near: %.60s\n", source, p);
            return;
        }
        char *one = cJSON_PrintUnformatted(node);
        br_node root = br_scene_file_load_json(e, one, strlen(one), source, br_scene_root(e));
        BR_CHECK(br_node_valid(e, root));
        /* components that act on their first frame (animator, tween) run once too */
        br_engine_step(e, 1.0f / 60.0f);
        br_engine_step(e, 1.0f / 60.0f);
        br_node_destroy(e, root);
        cJSON_free(one);
        cJSON_Delete(node);
        values++;
        p = end;
    }
    BR_CHECK(values > 0);
}

static void check_example(br_engine *e, const example *x)
{
    char source[64];
    SDL_snprintf(source, sizeof(source), "DATA_FORMATS.md:%d", x->line);
    g_problems = 0;
    size_t len = strlen(x->text);
    if (strcmp(x->kind, "game") == 0) {
        br_game_config config;
        BR_CHECK(br_game_config_parse(x->text, len, source, &config));
        br_game_config_free(&config);
    } else if (strcmp(x->kind, "scene") == 0 || strcmp(x->kind, "prefab") == 0) {
        load_nodes(e, x->text, source);
    } else if (strcmp(x->kind, "fields") == 0) {
        /* a fragment of a node object ("assets": [...], "filters": [...]): put it in a container
         * labelled "hero" (the label the component example points at) */
        size_t size = len + 64;
        char *wrapped = SDL_malloc(size);
        SDL_snprintf(wrapped, size, "{ \"type\": \"container\", \"label\": \"hero\", %s }", x->text);
        load_nodes(e, wrapped, source);
        SDL_free(wrapped);
    } else if (strcmp(x->kind, "manifest") == 0) {
        BR_CHECK(br_assets_add_manifest(e, x->text, len));
    } else if (strcmp(x->kind, "input") == 0) {
        cJSON *json = cJSON_Parse(x->text);
        BR_CHECK(json != NULL && br_input_actions_from_json(e, json));
        cJSON_Delete(json);
    } else if (strcmp(x->kind, "anim") == 0) {
        br_anim_library *lib = br_anim_library_parse(e, source, x->text, len);
        BR_CHECK(lib != NULL);
        br_anim_library_free(lib);
    } else if (strcmp(x->kind, "animgraph") == 0) {
        br_anim_graph *graph = br_anim_graph_parse(e, source, x->text, len);
        BR_CHECK(graph != NULL);
        br_anim_graph_free(graph);
    } else {
        BR_CHECK(!"unknown check kind");
        printf("%s: unknown check kind '%s'\n", source, x->kind);
    }
    if (g_problems != 0) {
        printf("%s (%s): %d warnings / errors, see the log above\n", source, x->kind, g_problems);
    }
    BR_CHECK(g_problems == 0);
}

/* The components the examples use (any game would register its own). */
typedef struct velocity { float x, y; } velocity;
typedef struct target { br_node node; } target;
typedef struct mover { br_vec2 velocity; float spin; } mover;
typedef struct enemy {
    bool     alive;
    int32_t  hp;
    float    speed;
    br_vec2  home;
    br_vec3  axis;
    br_color flash;
    char     name[BR_FIELD_STRING_SIZE];
    char     hit_sound[BR_FIELD_STRING_SIZE];
    br_node  target;
} enemy;

static void register_components(br_engine *e)
{
    static const br_field_desc velocity_fields[] = {
        { "x", BR_FIELD_FLOAT, offsetof(velocity, x) },
        { "y", BR_FIELD_FLOAT, offsetof(velocity, y) },
    };
    static const br_field_desc target_fields[] = { { "node", BR_FIELD_NODE, offsetof(target, node) } };
    static const br_field_desc mover_fields[] = {
        { "velocity", BR_FIELD_VEC2, offsetof(mover, velocity) },
        { "spin", BR_FIELD_FLOAT, offsetof(mover, spin) },
    };
    static const br_field_desc enemy_fields[] = {
        { "alive", BR_FIELD_BOOL, offsetof(enemy, alive) },
        { "hp", BR_FIELD_INT, offsetof(enemy, hp) },
        { "speed", BR_FIELD_FLOAT, offsetof(enemy, speed) },
        { "home", BR_FIELD_VEC2, offsetof(enemy, home) },
        { "axis", BR_FIELD_VEC3, offsetof(enemy, axis) },
        { "flash", BR_FIELD_COLOR, offsetof(enemy, flash) },
        { "name", BR_FIELD_STRING, offsetof(enemy, name) },
        { "hit_sound", BR_FIELD_ASSET, offsetof(enemy, hit_sound) },
        { "target", BR_FIELD_NODE, offsetof(enemy, target) },
    };
    br_component_desc d1 = { "velocity", sizeof(velocity), velocity_fields, BR_ARRAY_COUNT(velocity_fields) };
    br_component_desc d2 = { "target", sizeof(target), target_fields, BR_ARRAY_COUNT(target_fields) };
    br_component_desc d3 = { "mover", sizeof(mover), mover_fields, BR_ARRAY_COUNT(mover_fields) };
    br_component_desc d4 = { "enemy", sizeof(enemy), enemy_fields, BR_ARRAY_COUNT(enemy_fields) };
    br_component_register(e, &d1);
    br_component_register(e, &d2);
    br_component_register(e, &d3);
    br_component_register(e, &d4);
}

int main(void)
{
    char doc_path[1024];
    SDL_snprintf(doc_path, sizeof(doc_path), "%s../../docs/DATA_FORMATS.md", SDL_GetBasePath());
    size_t size = 0;
    char *doc = SDL_LoadFile(doc_path, &size);
    BR_CHECK(doc != NULL);
    if (doc == NULL) {
        printf("cannot read %s\n", doc_path);
        return br_test_finish();
    }
    static example examples[MAX_EXAMPLES];
    int fences = 0;
    int count = collect_examples(doc, examples, MAX_EXAMPLES, &fences);
    SDL_free(doc);
    printf("%d examples, %d json blocks\n", count, fences);
    BR_CHECK(count > 0 && count == fences); /* every example has a check line */

    char root[1024];
    SDL_snprintf(root, sizeof(root), "%sdoc_examples/", SDL_GetBasePath());
    SDL_CreateDirectory(root);
    stage_assets(root);
    for (int i = 0; i < count; i++) {
        BR_CHECK(examples[i].text != NULL);
        if (examples[i].text != NULL && examples[i].path[0] != '\0') {
            BR_CHECK(write_file(root, examples[i].path, examples[i].text, strlen(examples[i].text)));
        }
    }

    SDL_GetLogOutputFunction(&g_prev_log, &g_prev_log_user);
    SDL_SetLogOutputFunction(count_log, NULL);
    br_engine *e = br_engine_create("test_data_formats", "doc_examples"); /* relative to the exe */
    register_components(e);
    /* manifests first: the scene examples name its aliases and bundles */
    for (int i = 0; i < count; i++) {
        if (examples[i].text != NULL && strcmp(examples[i].kind, "manifest") == 0) {
            check_example(e, &examples[i]);
        }
    }
    for (int i = 0; i < count; i++) {
        if (examples[i].text != NULL && strcmp(examples[i].kind, "manifest") != 0) {
            check_example(e, &examples[i]);
        }
    }
    br_engine_destroy(e);
    SDL_SetLogOutputFunction(g_prev_log, g_prev_log_user);
    for (int i = 0; i < count; i++) {
        SDL_free(examples[i].text);
    }
    return br_test_finish();
}
