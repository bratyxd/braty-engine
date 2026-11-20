/* test_text_field - the text field node: typing, caret and deletion over multi-byte text,
 * selection (keys, mouse drag, double-click), clipboard-sized pastes, invalid UTF-8, undo, max
 * length, single-line scroll, multi-line, password mode, placeholder, IME composition (start,
 * update, commit, cancel, focus lost mid-composition), focus and keyboard capture, change /
 * submit events, scene files and save round trips. Headless. */
#include "braty/braty.h"
#include "br_test.h"

#include <SDL3/SDL.h>

static void step(br_engine *e)
{
    br_engine_step(e, 1.0f / 60.0f);
}

static void key(br_engine *e, SDL_Scancode k)
{
    br_input_inject_key(e, k, true);
    br_input_inject_key(e, k, false);
}

/* Presses k with a modifier held. */
static void chord(br_engine *e, SDL_Scancode mod, SDL_Scancode k)
{
    br_input_inject_key(e, mod, true);
    key(e, k);
    br_input_inject_key(e, mod, false);
}

static bool text_is(br_engine *e, br_node f, const char *expected)
{
    const char *t = br_text_field_text(e, f);
    if (SDL_strcmp(t, expected) != 0) {
        printf("  text is '%s', expected '%s'\n", t, expected);
        return false;
    }
    return true;
}

static uint32_t caret(br_engine *e, br_node f)
{
    uint32_t c = 0;
    br_text_field_get_selection(e, f, NULL, &c);
    return c;
}

/* True if every byte sequence decodes as UTF-8 and no code point is split. */
static bool valid_utf8(const char *s)
{
    size_t len = SDL_strlen(s);
    size_t off = 0;
    while (off < len) {
        size_t before = off;
        uint32_t cp = br_text_utf8_next(s, len, &off);
        if (cp == 0xFFFD && !(off - before == 3 && (unsigned char)s[before] == 0xEF)) {
            return false;   /* decoded as invalid (a real U+FFFD is EF BF BD) */
        }
    }
    return true;
}

typedef struct events_seen {
    int changed;
    int submitted;
    int focused;
    int blurred;
    bool blur_on_submit;
} events_seen;

static void on_field(br_engine *e, br_node field, br_text_field_event event, void *user)
{
    (void)field;
    events_seen *s = user;
    switch (event) {
    case BR_TEXT_FIELD_CHANGED: s->changed++; break;
    case BR_TEXT_FIELD_SUBMITTED:
        s->submitted++;
        if (s->blur_on_submit) {
            br_node none = { 0, 0 };
            br_text_field_focus(e, none);
        }
        break;
    case BR_TEXT_FIELD_FOCUSED: s->focused++; break;
    case BR_TEXT_FIELD_BLURRED: s->blurred++; break;
    }
}

static br_node make_field(br_engine *e, float x, float y, float w, float h)
{
    br_node f = br_text_field_create(e, br_vec2_make(w, h), NULL);
    br_node_set_position(e, f, br_vec2_make(x, y));
    br_node_add_child(e, br_stage_root(e), f);
    return f;
}

static void test_typing_and_utf8(br_engine *e)
{
    br_node f = make_field(e, 10, 10, 300, 40);
    events_seen seen;
    SDL_zero(seen);
    br_text_field_set_listener(e, f, on_field, &seen);
    br_input_inject_text(e, "no focus");
    BR_CHECK(text_is(e, f, ""));   /* text goes only to the focused field */
    br_text_field_focus(e, f);
    BR_CHECK(br_node_equal(br_text_field_focused(e), f) && seen.focused == 1);
    BR_CHECK(br_input_keyboard_captured(e));

    /* a (1 byte), e-acute (2), kanji (3), emoji (4), e + combining acute (1 + 2) */
    br_input_inject_text(e, "a\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80" "e\xCC\x81");
    BR_CHECK(text_is(e, f, "a\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80" "e\xCC\x81"));
    BR_CHECK(seen.changed == 1);
    uint32_t len = (uint32_t)SDL_strlen(br_text_field_text(e, f));
    BR_CHECK(caret(e, f) == len);
    /* Left walks code point boundaries: 13 -> 11 (combining mark) -> 10 -> 6 -> 3 -> 1 -> 0 */
    uint32_t expected[] = { 11, 10, 6, 3, 1, 0, 0 };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(expected); i++) {
        key(e, SDL_SCANCODE_LEFT);
        BR_CHECK(caret(e, f) == expected[i]);
    }
    uint32_t forward[] = { 1, 3, 6, 10, 11, 13, 13 };
    for (uint32_t i = 0; i < BR_ARRAY_COUNT(forward); i++) {
        key(e, SDL_SCANCODE_RIGHT);
        BR_CHECK(caret(e, f) == forward[i]);
    }
    /* Backspace removes whole code points (the combining mark first: grapheme clusters are a limit) */
    key(e, SDL_SCANCODE_BACKSPACE);
    BR_CHECK(text_is(e, f, "a\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80" "e"));
    key(e, SDL_SCANCODE_BACKSPACE);
    key(e, SDL_SCANCODE_BACKSPACE);
    BR_CHECK(text_is(e, f, "a\xC3\xA9\xE6\xBC\xA2"));
    key(e, SDL_SCANCODE_HOME);
    key(e, SDL_SCANCODE_DELETE);
    key(e, SDL_SCANCODE_DELETE);
    BR_CHECK(text_is(e, f, "\xE6\xBC\xA2") && caret(e, f) == 0);
    /* offsets set from code snap back to a code point start */
    br_text_field_set_selection(e, f, 2, 1);
    uint32_t a = 9;
    uint32_t c = 9;
    br_text_field_get_selection(e, f, &a, &c);
    BR_CHECK(a == 0 && c == 0);

    /* invalid UTF-8 typed or set: replaced, the buffer stays valid; controls dropped */
    br_text_field_set_text(e, f, "");
    br_input_inject_text(e, "x\xFF" "y\xC3" "z\xE6\xBC\x01" "\t");
    BR_CHECK(text_is(e, f, "x\xEF\xBF\xBDy\xEF\xBF\xBDz\xEF\xBF\xBD\xEF\xBF\xBD "));
    br_text_field_set_text(e, f, "\xC0\xAF\xED\xA0\x80ok");   /* overlong '/', a surrogate */
    BR_CHECK(SDL_strstr(br_text_field_text(e, f), "ok") != NULL);
    BR_CHECK(SDL_strchr(br_text_field_text(e, f), '/') == NULL);
    br_node_destroy(e, f);
    BR_CHECK(!br_input_keyboard_captured(e));   /* destroying the focused field releases the keyboard */
    BR_CHECK(!br_node_valid(e, br_text_field_focused(e)));
    step(e);
}

static void test_selection_and_words(br_engine *e)
{
    br_node f = make_field(e, 10, 10, 400, 40);
    br_text_field_focus(e, f);
    br_input_inject_text(e, "hello big world");
    chord(e, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LEFT);
    chord(e, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LEFT);
    uint32_t a = 0;
    uint32_t c = 0;
    br_text_field_get_selection(e, f, &a, &c);
    BR_CHECK(a == 15 && c == 13);
    br_input_inject_text(e, "LD");   /* typing replaces the selection */
    BR_CHECK(text_is(e, f, "hello big worLD"));
    chord(e, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LEFT);
    BR_CHECK(caret(e, f) == 10);
    chord(e, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LEFT);
    BR_CHECK(caret(e, f) == 6);
    chord(e, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RIGHT);
    BR_CHECK(caret(e, f) == 10);
    chord(e, SDL_SCANCODE_LCTRL, SDL_SCANCODE_BACKSPACE);
    BR_CHECK(text_is(e, f, "hello worLD"));
    chord(e, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A);
    br_text_field_get_selection(e, f, &a, &c);
    BR_CHECK(a == 0 && c == 11);
    key(e, SDL_SCANCODE_LEFT);   /* Left with a selection collapses it to its start */
    BR_CHECK(caret(e, f) == 0);
    chord(e, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_END);
    br_text_field_get_selection(e, f, &a, &c);
    BR_CHECK(a == 0 && c == 11);
    key(e, SDL_SCANCODE_DELETE);
    BR_CHECK(text_is(e, f, ""));
    br_node_destroy(e, f);
    step(e);
}

static void test_mouse(br_engine *e)
{
    br_node f = make_field(e, 100, 100, 400, 40);
    br_node other = make_field(e, 100, 300, 400, 40);
    br_text_field_set_text(e, f, "alpha beta gamma");
    step(e);
    br_rect r;
    /* the caret goes where the click is */
    br_text_field_set_selection(e, f, 6, 6);
    BR_CHECK(br_text_field_get_caret_rect(e, f, &r));
    float beta_x = 100.0f + r.x;
    br_text_field_set_selection(e, f, 0, 0);
    br_input_inject_pointer_move(e, beta_x + 1.0f, 120);
    br_input_inject_pointer_down(e, beta_x + 1.0f, 120, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, beta_x + 1.0f, 120, SDL_BUTTON_LEFT);
    step(e);
    BR_CHECK(br_node_equal(br_text_field_focused(e), f));
    BR_CHECK(caret(e, f) == 6);
    BR_CHECK(br_text_field_offset_at(e, f, br_vec2_make(-50, 20)) == 0);
    BR_CHECK(br_text_field_offset_at(e, f, br_vec2_make(399, 20)) == 16);

    /* double-click selects the word */
    br_input_inject_pointer_down(e, beta_x + 14.0f, 120, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, beta_x + 14.0f, 120, SDL_BUTTON_LEFT);
    br_input_inject_pointer_down(e, beta_x + 14.0f, 120, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, beta_x + 14.0f, 120, SDL_BUTTON_LEFT);
    uint32_t a = 0;
    uint32_t c = 0;
    br_text_field_get_selection(e, f, &a, &c);
    BR_CHECK(a == 6 && c == 10);
    step(e);

    /* drag from the start to past the end selects everything */
    br_input_inject_pointer_move(e, 101, 300 + 20);
    step(e);
    br_input_inject_pointer_move(e, 101, 120);
    br_input_inject_pointer_down(e, 101, 120, SDL_BUTTON_LEFT);
    step(e);
    br_input_inject_pointer_move(e, 600, 125);
    step(e);
    br_input_inject_pointer_up(e, 600, 125, SDL_BUTTON_LEFT);
    step(e);
    br_text_field_get_selection(e, f, &a, &c);
    BR_CHECK(a == 0 && c == 16);

    /* a press on the other field moves focus, a press on nothing blurs */
    br_input_inject_pointer_move(e, 150, 320);
    br_input_inject_pointer_down(e, 150, 320, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 150, 320, SDL_BUTTON_LEFT);
    step(e);
    BR_CHECK(br_node_equal(br_text_field_focused(e), other));
    br_input_inject_pointer_move(e, 900, 600);
    br_input_inject_pointer_down(e, 900, 600, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 900, 600, SDL_BUTTON_LEFT);
    step(e);
    BR_CHECK(!br_node_valid(e, br_text_field_focused(e)));
    BR_CHECK(!br_input_keyboard_captured(e));
    br_node_destroy(e, f);
    br_node_destroy(e, other);
    step(e);
}

/* Review Focus: a 1 MB paste is bounded by max length, costs linear time and keeps valid UTF-8. */
static void test_big_paste(br_engine *e)
{
    br_node f = make_field(e, 10, 10, 300, 40);
    br_text_field_set_max_length(e, f, 5);
    br_text_field_focus(e, f);
    br_input_inject_text(e, "abcdefgh");
    BR_CHECK(text_is(e, f, "abcde"));
    br_input_inject_text(e, "x");   /* full: nothing happens */
    BR_CHECK(text_is(e, f, "abcde"));
    chord(e, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LEFT);
    br_input_inject_text(e, "\xE6\xBC\xA2\xE6\xBC\xA2");   /* replaces the selection, cut at a code point */
    BR_CHECK(text_is(e, f, "abcd\xE6\xBC\xA2"));

    size_t size = 1024 * 1024;
    char *big = SDL_malloc(size + 1);
    for (size_t i = 0; i < size; i++) {
        /* a 3-byte character, then a stray continuation byte every 7th position */
        size_t k = i % 7;
        big[i] = k == 0 ? '\xE6' : k == 1 ? '\xBC' : k == 2 ? '\xA2' : k == 3 ? '\x80' : (char)('a' + (char)k);
    }
    big[size - 1] = '\xE6';   /* ends in the middle of a character */
    big[size] = '\0';
    br_text_field_set_max_length(e, f, BR_TEXT_FIELD_MAX_LENGTH);
    br_text_field_select_all(e, f);
    uint64_t t0 = SDL_GetTicksNS();
    BR_CHECK(br_text_field_insert(e, f, big));
    uint64_t t1 = SDL_GetTicksNS();
    BR_CHECK(valid_utf8(br_text_field_text(e, f)));
    size_t len = SDL_strlen(br_text_field_text(e, f));
    uint32_t cps = 0;
    for (size_t i = 0; i < len; i++) {
        cps += ((unsigned char)br_text_field_text(e, f)[i] & 0xC0u) != 0x80u ? 1u : 0u;
    }
    BR_CHECK(cps == BR_TEXT_FIELD_MAX_LENGTH);
    BR_CHECK(t1 - t0 < 2000000000ull);   /* linear: well under a second here */
    step(e);   /* draws nothing headless, but the update system runs over the big text */
    /* a second paste into a full field is refused without work */
    br_text_field_set_selection(e, f, 0, 0);
    BR_CHECK(!br_text_field_insert(e, f, big));
    SDL_free(big);
    /* the clipboard path, where SDL has one (headless runs may not) */
    if (SDL_SetClipboardText("clip\xFFped")) {
        br_text_field_set_max_length(e, f, 100);
        br_text_field_set_text(e, f, "");
        BR_CHECK(br_text_field_paste(e, f));
        BR_CHECK(SDL_strncmp(br_text_field_text(e, f), "clip", 4) == 0);
        BR_CHECK(valid_utf8(br_text_field_text(e, f)));
    }
    br_node_destroy(e, f);
    step(e);
}

static void test_undo(br_engine *e)
{
    br_node f = make_field(e, 10, 10, 300, 40);
    events_seen seen;
    SDL_zero(seen);
    br_text_field_set_listener(e, f, on_field, &seen);
    br_text_field_focus(e, f);
    BR_CHECK(!br_text_field_undo(e, f));
    br_input_inject_text(e, "a");
    br_input_inject_text(e, "b");
    br_input_inject_text(e, "c");   /* one run of typing = one undo step */
    BR_CHECK(seen.changed == 3);
    chord(e, SDL_SCANCODE_LCTRL, SDL_SCANCODE_Z);
    BR_CHECK(text_is(e, f, "") && seen.changed == 4);
    chord(e, SDL_SCANCODE_LCTRL, SDL_SCANCODE_Z);   /* again = redo */
    BR_CHECK(text_is(e, f, "abc") && caret(e, f) == 3);
    br_text_field_insert(e, f, "XYZ");
    BR_CHECK(br_text_field_undo(e, f) && text_is(e, f, "abc"));
    /* moving the caret ends a run */
    br_text_field_set_text(e, f, "");
    br_input_inject_text(e, "a");
    key(e, SDL_SCANCODE_LEFT);
    br_input_inject_text(e, "b");
    BR_CHECK(text_is(e, f, "ba"));
    BR_CHECK(br_text_field_undo(e, f) && text_is(e, f, "a"));
    /* set_text clears undo */
    br_text_field_set_text(e, f, "fresh");
    BR_CHECK(!br_text_field_undo(e, f));
    br_node_destroy(e, f);
    step(e);
}

/* Review Focus: IME composition start, update, commit, cancel and focus lost mid-composition. */
static void test_ime(br_engine *e)
{
    br_node f = make_field(e, 10, 10, 300, 40);
    events_seen seen;
    SDL_zero(seen);
    br_text_field_set_listener(e, f, on_field, &seen);
    br_text_field_focus(e, f);
    br_input_inject_text(e, "ab");
    key(e, SDL_SCANCODE_LEFT);   /* compose between a and b */
    int changed = seen.changed;
    br_rect before;
    br_text_field_get_caret_rect(e, f, &before);

    br_input_inject_text_editing(e, "k", 1, 0);   /* start */
    BR_CHECK(SDL_strcmp(br_text_field_composition(e, f), "k") == 0);
    BR_CHECK(text_is(e, f, "ab") && seen.changed == changed);   /* pre-edit text is not the field's text */
    br_input_inject_text_editing(e, "\xE3\x81\x8B\xE3\x82\x93", 2, 0);   /* update: kana, cursor at its end */
    BR_CHECK(SDL_strcmp(br_text_field_composition(e, f), "\xE3\x81\x8B\xE3\x82\x93") == 0);
    br_rect during;
    br_text_field_get_caret_rect(e, f, &during);
    BR_CHECK(during.x > before.x);   /* the caret sits after the pre-edit text */
    br_input_inject_text(e, "\xE6\xBC\xA2");   /* commit */
    BR_CHECK(br_text_field_composition(e, f)[0] == '\0');
    BR_CHECK(text_is(e, f, "a\xE6\xBC\xA2" "b") && caret(e, f) == 4);
    BR_CHECK(seen.changed == changed + 1);

    br_input_inject_text_editing(e, "zz", 2, 0);
    br_input_inject_text_editing(e, "", 0, 0);   /* cancel */
    BR_CHECK(br_text_field_composition(e, f)[0] == '\0' && text_is(e, f, "a\xE6\xBC\xA2" "b"));

    /* invalid bytes in the pre-edit text are replaced too */
    br_input_inject_text_editing(e, "q\xFF", -1, -1);
    BR_CHECK(SDL_strcmp(br_text_field_composition(e, f), "q\xEF\xBF\xBD") == 0);
    /* focus lost mid-composition: the pre-edit text is dropped, the text is untouched */
    br_node none = { 0, 0 };
    br_text_field_focus(e, none);
    BR_CHECK(br_text_field_composition(e, f)[0] == '\0' && text_is(e, f, "a\xE6\xBC\xA2" "b"));
    BR_CHECK(seen.blurred == 1);
    br_input_inject_text(e, "late");   /* a commit arriving after the blur goes nowhere */
    BR_CHECK(text_is(e, f, "a\xE6\xBC\xA2" "b"));

    /* a composition replaces the selection; password fields ignore composition */
    br_text_field_focus(e, f);
    br_text_field_select_all(e, f);
    br_input_inject_text_editing(e, "x", 1, 0);
    BR_CHECK(text_is(e, f, ""));
    br_input_inject_text(e, "x");
    BR_CHECK(text_is(e, f, "x"));
    br_text_field_set_password(e, f, true);
    br_input_inject_text_editing(e, "y", 1, 0);
    BR_CHECK(br_text_field_composition(e, f)[0] == '\0');
    br_node_destroy(e, f);
    step(e);
}

/* Review Focus: typed keys do not reach game actions; the Enter that submits does not press
 * "confirm" either. */
static void test_capture_and_submit(br_engine *e)
{
    br_input_action_bind_key(e, "jump", SDL_SCANCODE_SPACE);
    br_input_action_bind_key(e, "confirm", SDL_SCANCODE_RETURN);
    br_node f = make_field(e, 10, 10, 300, 40);
    events_seen seen;
    SDL_zero(seen);
    seen.blur_on_submit = true;
    br_text_field_set_listener(e, f, on_field, &seen);
    br_text_field_focus(e, f);
    br_input_inject_key(e, SDL_SCANCODE_SPACE, true);
    br_input_inject_text(e, " ");
    step(e);
    BR_CHECK(text_is(e, f, " "));
    BR_CHECK(!br_input_action_pressed(e, "jump") && !br_input_action_down(e, "jump"));
    br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
    br_input_inject_key(e, SDL_SCANCODE_RETURN, true);   /* submits; the listener blurs */
    BR_CHECK(seen.submitted == 1 && seen.blurred == 1);
    BR_CHECK(!br_input_keyboard_captured(e));
    step(e);
    BR_CHECK(!br_input_action_pressed(e, "confirm") && !br_input_action_down(e, "confirm"));
    step(e);
    BR_CHECK(!br_input_action_down(e, "confirm"));
    br_input_inject_key(e, SDL_SCANCODE_RETURN, false);
    step(e);
    br_input_inject_key(e, SDL_SCANCODE_SPACE, true);   /* not focused: the game gets its keys back */
    step(e);
    BR_CHECK(br_input_action_pressed(e, "jump"));
    br_input_inject_key(e, SDL_SCANCODE_SPACE, false);
    /* Escape blurs */
    br_text_field_focus(e, f);
    key(e, SDL_SCANCODE_ESCAPE);
    BR_CHECK(!br_node_valid(e, br_text_field_focused(e)));
    br_input_actions_clear(e);
    br_node_destroy(e, f);
    step(e);
}

static void test_scroll_multiline_password(br_engine *e)
{
    /* single line: the caret stays inside the box */
    br_node f = make_field(e, 10, 10, 120, 40);
    br_text_field_focus(e, f);
    br_input_inject_text(e, "a fairly long line that cannot fit in the box");
    br_rect r;
    BR_CHECK(br_text_field_get_caret_rect(e, f, &r));
    BR_CHECK(r.x > 60.0f && r.x + r.width <= 120.0f);
    key(e, SDL_SCANCODE_HOME);
    BR_CHECK(br_text_field_get_caret_rect(e, f, &r));
    BR_CHECK(r.x >= 0.0f && r.x < 20.0f);
    /* Enter submits instead of inserting; pasted newlines become spaces */
    key(e, SDL_SCANCODE_RETURN);
    br_text_field_set_text(e, f, "one\ntwo");
    BR_CHECK(text_is(e, f, "one two"));

    /* multi-line: Enter inserts a newline, Up / Down move between lines, long lines wrap */
    br_text_field_set_multiline(e, f, true);
    br_text_field_set_size(e, f, br_vec2_make(200, 200));
    br_text_field_set_text(e, f, "");
    br_input_inject_text(e, "top");
    key(e, SDL_SCANCODE_RETURN);
    br_input_inject_text(e, "bottom");
    BR_CHECK(text_is(e, f, "top\nbottom"));
    br_rect bottom;
    br_text_field_get_caret_rect(e, f, &bottom);
    key(e, SDL_SCANCODE_UP);
    BR_CHECK(caret(e, f) == 3);   /* end of "top": nearest to the end of "bottom" */
    br_text_field_get_caret_rect(e, f, &r);
    BR_CHECK(r.y < bottom.y);
    key(e, SDL_SCANCODE_DOWN);
    BR_CHECK(caret(e, f) > 4);
    key(e, SDL_SCANCODE_HOME);
    BR_CHECK(caret(e, f) == 4);
    chord(e, SDL_SCANCODE_LCTRL, SDL_SCANCODE_END);
    BR_CHECK(caret(e, f) == 10);
    br_text_field_set_text(e, f, "word word word word word word word word word word");
    br_text_field_get_caret_rect(e, f, &r);
    BR_CHECK(r.y > 40.0f);   /* wrapped onto later lines */
    br_text_field_set_text(e, f, "\n\n");   /* empty lines get carets too */
    br_text_field_set_selection(e, f, 1, 1);
    br_text_field_get_caret_rect(e, f, &r);
    br_rect line2;
    br_text_field_set_selection(e, f, 2, 2);
    br_text_field_get_caret_rect(e, f, &line2);
    BR_CHECK(line2.y > r.y);

    /* password: bullets on screen, the real text in the API, no copy */
    br_text_field_set_multiline(e, f, false);
    br_text_field_set_password(e, f, true);
    br_text_field_set_text(e, f, "s3cr\xC3\xA9t");
    BR_CHECK(text_is(e, f, "s3cr\xC3\xA9t"));
    br_text_field_select_all(e, f);
    BR_CHECK(!br_text_field_copy(e, f) && !br_text_field_cut(e, f));
    BR_CHECK(text_is(e, f, "s3cr\xC3\xA9t"));
    key(e, SDL_SCANCODE_LEFT);
    key(e, SDL_SCANCODE_RIGHT);
    key(e, SDL_SCANCODE_BACKSPACE);
    BR_CHECK(text_is(e, f, "3cr\xC3\xA9t"));   /* Left collapsed the selection to 0, Right went past 's' */

    /* placeholder: shown while empty, never part of the text */
    br_text_field_set_password(e, f, false);
    br_text_field_set_text(e, f, "");
    br_text_field_set_placeholder(e, f, "Your name");
    BR_CHECK(text_is(e, f, "") && SDL_strcmp(br_text_field_placeholder(e, f), "Your name") == 0);
    BR_CHECK(br_text_field_get_caret_rect(e, f, &r) && r.x < 20.0f);

    /* bad values are refused, logged */
    br_text_field_set_max_length(e, f, 0);
    br_text_field_set_size(e, f, br_vec2_make(-5, 10));
    br_text_field_set_padding(e, f, -1);
    BR_CHECK(br_text_field_max_length(e, f) == 256 && br_text_field_size(e, f).x == 200.0f &&
             br_text_field_padding(e, f) == 8.0f);
    br_node_destroy(e, f);
    step(e);
}

static void test_scene_file(br_engine *e)
{
    const char *json =
        "{ \"type\": \"container\", \"children\": [ { \"type\": \"text_field\", \"label\": \"name\","
        " \"position\": [20, 30], \"size\": [240, 44], \"text\": \"Ada\", \"placeholder\": \"Name\","
        " \"max_length\": 12, \"padding\": 6, \"password\": false, \"multiline\": false,"
        " \"style\": { \"font_size\": 20, \"fill\": \"#202020\" },"
        " \"colors\": { \"background\": \"#ffffff\", \"selection\": \"#9cc4ff\", \"caret\": \"black\" } } ] }";
    br_node root = br_scene_file_load_json(e, json, SDL_strlen(json), "test", br_stage_root(e));
    br_node f = br_node_find_by_label(e, root, "name", true);
    BR_CHECK(br_node_type_of(e, f) == br_text_field_type(e));
    BR_CHECK(text_is(e, f, "Ada") && br_text_field_max_length(e, f) == 12);
    BR_CHECK(br_text_field_size(e, f).x == 240.0f && br_text_field_padding(e, f) == 6.0f);
    BR_CHECK(SDL_strcmp(br_text_field_placeholder(e, f), "Name") == 0);
    br_text_style st;
    BR_CHECK(br_text_field_get_style(e, f, &st) && st.font_size == 20.0f);
    /* saved JSON carries the live text; a reload does not double the field's own children */
    br_text_field_set_text(e, f, "Grace");
    cJSON *saved = br_scene_file_to_json(e, root);
    char *text = cJSON_PrintUnformatted(saved);
    BR_CHECK(SDL_strstr(text, "\"Grace\"") != NULL);
    br_node copy = br_scene_file_load_json(e, text, SDL_strlen(text), "saved", br_stage_root(e));
    step(e);
    step(e);   /* destroyed in one frame, freed at the start of the next */
    br_node f2 = br_node_find_by_label(e, copy, "name", true);
    BR_CHECK(text_is(e, f2, "Grace"));
    BR_CHECK(br_node_child_count(e, f2) == 2);
    cJSON_free(text);
    cJSON_Delete(saved);
    /* bad values log and are ignored */
    const char *bad = "{ \"type\": \"text_field\", \"max_length\": 0, \"size\": [0, 10], \"text\": 5 }";
    uint32_t errors = br_scene_file_error_count(e);
    br_node b = br_scene_file_load_json(e, bad, SDL_strlen(bad), "bad", br_stage_root(e));
    BR_CHECK(br_node_valid(e, b) && br_scene_file_error_count(e) > errors);
    BR_CHECK(br_text_field_max_length(e, b) == 256);
    br_node_destroy(e, b);
    br_node_destroy(e, copy);
    br_node_destroy(e, root);
    step(e);
}

/* A field on a canvas3d is clicked through the 3D pick, keeps focus and takes typing. */
static void test_canvas3d(br_engine *e)
{
    br_node view = br_view3d_create(e, 200.0f, 200.0f);
    br_node_add_child(e, br_stage_root(e), view);
    br_node3d cam = br_view3d_camera(e, view);
    br_node3d_set_position(e, cam, br_vec3_make(0.0f, 0.0f, 10.0f));
    br_node3d_look_at(e, cam, br_vec3_make(0.0f, 0.0f, 0.0f), br_vec3_make(0.0f, 1.0f, 0.0f));
    br_canvas3d_desc d = br_canvas3d_default(200.0f, 100.0f);
    br_node3d canvas = br_canvas3d_create(e, &d);
    br_node3d_add_child(e, br_view3d_root(e, view), canvas);
    br_node f = br_text_field_create(e, br_vec2_make(160, 40), NULL);
    br_node_set_position(e, f, br_vec2_make(20, 30));
    br_node_add_child(e, br_canvas3d_root(e, canvas), f);
    step(e);
    br_input_inject_pointer_move(e, 100.0f, 100.0f);   /* the canvas centre = 2D (100, 50): inside the field */
    br_input_inject_pointer_down(e, 100.0f, 100.0f, SDL_BUTTON_LEFT);
    br_input_inject_pointer_up(e, 100.0f, 100.0f, SDL_BUTTON_LEFT);
    step(e);
    BR_CHECK(br_node_equal(br_text_field_focused(e), f));
    br_input_inject_text(e, "3D");
    BR_CHECK(text_is(e, f, "3D"));
    br_node3d_destroy(e, canvas);
    br_node_destroy(e, view);
    BR_CHECK(!br_input_keyboard_captured(e));
    step(e);
}

int main(void)
{
    br_engine *e = br_engine_create("test_text_field", NULL);
    if (e == NULL) {
        BR_CHECK(false);
        return br_test_finish();
    }
    test_typing_and_utf8(e);
    test_selection_and_words(e);
    test_mouse(e);
    test_big_paste(e);
    test_undo(e);
    test_ime(e);
    test_capture_and_submit(e);
    test_scroll_multiline_password(e);
    test_scene_file(e);
    test_canvas3d(e);
    br_engine_destroy(e);
    return br_test_finish();
}
