/* test_filters - colour-matrix presets, blur kernel weights, render-target pool, filter lists (headless). */
#include "braty/braty.h"
#include "br_test.h"

static bool near(float a, float b)
{
    return SDL_fabsf(a - b) < 1e-4f;
}

static bool color_near(br_color a, br_color b)
{
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

static bool matrix_is_identity(const float m[20])
{
    float id[20];
    br_color_matrix_identity(id);
    for (int i = 0; i < 20; i++) {
        if (!near(m[i], id[i])) {
            return false;
        }
    }
    return true;
}

static void test_color_matrix(void)
{
    float m[20];
    br_color c = br_color_make(0.2f, 0.5f, 0.8f, 1.0f);

    br_color_matrix_identity(m);
    BR_CHECK(color_near(br_color_matrix_apply(m, c), c));

    br_color_matrix_brightness(m, 0.5f, false);
    BR_CHECK(color_near(br_color_matrix_apply(m, c), br_color_make(0.1f, 0.25f, 0.4f, 1.0f)));
    /* multiply combines: 0.5 then 2 = identity */
    br_color_matrix_brightness(m, 2.0f, true);
    BR_CHECK(matrix_is_identity(m));

    br_color_matrix_negative(m, false);
    BR_CHECK(color_near(br_color_matrix_apply(m, c), br_color_make(0.8f, 0.5f, 0.2f, 1.0f)));

    br_color_matrix_greyscale(m, 1.0f / 3.0f, false);
    br_color g = br_color_matrix_apply(m, c);
    BR_CHECK(near(g.r, 0.5f) && near(g.g, 0.5f) && near(g.b, 0.5f));

    /* neutral settings are identities */
    br_color_matrix_hue(m, 0.0f, false);
    BR_CHECK(matrix_is_identity(m));
    br_color_matrix_saturate(m, 0.0f, false);
    BR_CHECK(matrix_is_identity(m));
    br_color_matrix_contrast(m, 0.0f, false);
    BR_CHECK(matrix_is_identity(m));

    /* hue keeps grey grey and 360 degrees comes back */
    br_color grey = br_color_make(0.4f, 0.4f, 0.4f, 1.0f);
    br_color_matrix_hue(m, 120.0f, false);
    BR_CHECK(color_near(br_color_matrix_apply(m, grey), grey));
    /* 120 degrees rotates red into green */
    br_color red_rot = br_color_matrix_apply(m, br_color_make(1.0f, 0.0f, 0.0f, 1.0f));
    BR_CHECK(near(red_rot.r, 0.0f) && near(red_rot.g, 1.0f) && near(red_rot.b, 0.0f));
    br_color_matrix_hue(m, 360.0f, false);
    BR_CHECK(matrix_is_identity(m));

    /* desaturate: every channel = the average-ish grey */
    br_color_matrix_desaturate(m);
    br_color d = br_color_matrix_apply(m, c);
    BR_CHECK(near(d.r, d.g) && near(d.g, d.b));

    /* contrast pivots around 0.5 */
    br_color_matrix_contrast(m, 1.0f, false);
    br_color half = br_color_matrix_apply(m, br_color_make(0.5f, 0.5f, 0.5f, 1.0f));
    BR_CHECK(near(half.r, 0.5f));
    BR_CHECK(near(br_color_matrix_apply(m, br_color_make(0.75f, 0.0f, 0.0f, 1.0f)).r, 1.0f));

    /* sepia rows (Pixi values) */
    br_color_matrix_sepia(m, false);
    br_color s = br_color_matrix_apply(m, br_color_make(0.5f, 0.5f, 0.5f, 1.0f));
    BR_CHECK(near(s.b, 0.5f * (0.272f + 0.5339999f + 0.13099999f)));

    /* to_bgr twice = identity; multiply order: m x preset */
    br_color_matrix_to_bgr(m, false);
    br_color_matrix_to_bgr(m, true);
    BR_CHECK(matrix_is_identity(m));
    float a[20], b[20], ab[20];
    br_color_matrix_brightness(a, 0.5f, false);
    br_color_matrix_negative(b, false);
    br_color_matrix_multiply(ab, a, b); /* negative first, then brightness */
    br_color r = br_color_matrix_apply(ab, br_color_make(0.2f, 0.2f, 0.2f, 1.0f));
    BR_CHECK(near(r.r, 0.4f));

    /* every preset leaves alpha alone (alpha row = identity) */
    br_color_matrix_technicolor(m, false);
    BR_CHECK(near(m[18], 1.0f) && near(m[15], 0.0f));
    br_color_matrix_lsd(m, false);
    BR_CHECK(near(m[18], 1.0f));
}

static void test_blur_kernel(void)
{
    for (int k = 5; k <= 15; k += 2) {
        float w[8];
        uint32_t n = br_filter_blur_kernel(k, w);
        BR_CHECK(n == (uint32_t)(k / 2 + 1));
        float sum = w[0];
        for (uint32_t i = 1; i < n; i++) {
            sum += 2.0f * w[i];
            BR_CHECK(w[i] < w[i - 1]); /* falls off from the centre */
        }
        BR_CHECK(near(sum, 1.0f));
    }
    float w[8];
    BR_CHECK(br_filter_blur_kernel(2, w) == 3);   /* clamped up to 5 taps */
    BR_CHECK(br_filter_blur_kernel(99, w) == 8);  /* clamped down to 15 taps */
    BR_CHECK(br_filter_blur_kernel(6, w) == 4);   /* even -> next odd */
}

static void test_pool(br_engine *e)
{
    BR_CHECK(br_filters_pool_bucket(1) == 16);
    BR_CHECK(br_filters_pool_bucket(16) == 16);
    BR_CHECK(br_filters_pool_bucket(17) == 32);
    BR_CHECK(br_filters_pool_bucket(300) == 512);
    BR_CHECK(br_filters_pool_bucket(1000000) == BR_FILTER_MAX_SIZE);

    br_texture a = br_filters_pool_acquire(e, 100, 60);
    br_texture b = br_filters_pool_acquire(e, 120, 50); /* same bucket 128x64, but a is in use */
    BR_CHECK(a.generation != 0 && b.generation != 0);
    BR_CHECK(!(a.index == b.index && a.generation == b.generation));
    br_texture_info info;
    BR_CHECK(br_texture_get_info(e, a, &info) && info.source_width == 128 && info.source_height == 64);
    BR_CHECK(br_filters_pool_count(e) == 2);

    /* next frame: both free again, the same textures come back */
    br_filters_pool_release_all(e);
    br_texture c = br_filters_pool_acquire(e, 128, 64);
    BR_CHECK(c.index == a.index || c.index == b.index);
    BR_CHECK(br_filters_pool_count(e) == 2);
    br_texture d = br_filters_pool_acquire(e, 20, 20); /* other bucket: new texture */
    BR_CHECK(br_filters_pool_count(e) == 3);
    BR_CHECK(br_texture_get_info(e, d, &info) && info.source_width == 32);

    /* trim destroys only free textures */
    br_filters_pool_trim(e);
    BR_CHECK(br_filters_pool_count(e) == 2);
    BR_CHECK(br_texture_valid(e, c) && br_texture_valid(e, d));
    br_filters_pool_release_all(e);
    br_filters_pool_trim(e);
    BR_CHECK(br_filters_pool_count(e) == 0);
    BR_CHECK(!br_texture_valid(e, c));
}

static void test_filter_lists(br_engine *e)
{
    br_node n = br_node_create_container(e);
    br_node_add_child(e, br_stage_root(e), n);
    BR_CHECK(!br_node_needs_effects(e, n));
    br_filter list[2] = { br_filter_blur(8.0f, 4, 5), br_filter_alpha(0.5f) };
    BR_CHECK(br_filters_set(e, n, list, 2));
    BR_CHECK(br_filters_count(e, n) == 2);
    BR_CHECK(br_node_needs_effects(e, n));
    br_filter *f = br_filters_get(e, n, 1);
    BR_CHECK(f != NULL && f->type == BR_FILTER_ALPHA && near(f->alpha, 0.5f));
    f->alpha = 0.25f; /* edit in place */
    BR_CHECK(near(br_filters_get(e, n, 1)->alpha, 0.25f));
    BR_CHECK(br_filters_get(e, n, 2) == NULL);
    BR_CHECK(near(br_filters_get(e, n, 0)->padding, 8.0f));
    BR_CHECK(!br_filters_set(e, n, list, BR_FILTER_MAX_PER_NODE + 1));
    BR_CHECK(br_filters_set(e, n, NULL, 0));
    BR_CHECK(br_filters_count(e, n) == 0);
    BR_CHECK(!br_node_needs_effects(e, n));

    /* custom filters copy their uniforms */
    float u[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    br_filter custom = br_filter_custom(NULL, u, sizeof(u));
    BR_CHECK(custom.uniform_size == sizeof(u));
    u[0] = 9.0f;
    BR_CHECK(near(((float *)custom.uniforms)[0], 1.0f));

    /* destroying the node drops its list; a stale handle reads as empty */
    BR_CHECK(br_filters_set(e, n, list, 1));
    br_node_destroy(e, n);
    BR_CHECK(br_filters_count(e, n) == 0);
    BR_CHECK(br_filters_get(e, n, 0) == NULL);
}

int main(void)
{
    test_color_matrix();
    test_blur_kernel();
    br_engine *e = br_engine_create("test_filters", NULL);
    BR_CHECK(e != NULL);
    if (e != NULL) {
        test_pool(e);
        test_filter_lists(e);
        br_engine_destroy(e);
    }
    return br_test_finish();
}
