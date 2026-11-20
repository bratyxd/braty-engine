/* br_profiler_save: statistics over the ring (percentiles, top scopes), the text and JSON
 * summaries, and the Chrome trace-event file (chrome://tracing, ui.perfetto.dev). */
#include "profiler/br_profiler_internal.h"

#include "cjson/cJSON.h"

#include <stdarg.h>

/* Numbers over the last n frames of the ring. */
typedef struct br_profiler_stats {
    uint32_t n;
    float    frame_ms[BR_PROFILER_FRAMES];   /* sorted after br_profiler_stats_compute */
    float    cpu_ms[BR_PROFILER_FRAMES];
    double   frame_sum, cpu_sum, update_sum, render_sum, acquire_sum, present_sum, pace_sum, gpu_sum;
    uint32_t gpu_n;
    double   self_ns[BR_PROFILER_MAX_NAMES];
    double   total_ns[BR_PROFILER_MAX_NAMES];
    uint32_t calls[BR_PROFILER_MAX_NAMES];
    br_profiler_verdict verdict;
} br_profiler_stats;

static int SDLCALL br_profiler_float_cmp(const void *a, const void *b)
{
    float x = *(const float *)a;
    float y = *(const float *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Nearest-rank percentile of a sorted array. */
static float br_profiler_percentile(const float *sorted, uint32_t n, float pct)
{
    if (n == 0) {
        return 0.0f;
    }
    uint32_t rank = (uint32_t)SDL_ceilf(pct / 100.0f * (float)n);
    return sorted[rank > 0 ? rank - 1 : 0];
}

static bool br_profiler_stats_compute(br_engine *e, uint32_t frames, br_profiler_stats *st)
{
    br_profiler_state *p = e->profiler;
    SDL_zerop(st);
    uint64_t self[BR_PROFILER_MAX_SCOPES];
    uint32_t want = frames == 0 || frames > BR_PROFILER_FRAMES ? BR_PROFILER_FRAMES : frames;
    /* the newest frame has no interval yet: start at the first one that does */
    for (uint32_t age = 0; st->n < want; age++) {
        br_profiler_frame *f = br_profiler_frame_at(p, age);
        if (f == NULL) {
            break;
        }
        if (f->interval_ns == 0) {
            continue;
        }
        uint64_t wall = f->end_ns - f->start_ns;
        uint64_t waits = f->acquire_ns + f->present_ns + f->pace_ns;
        float cpu = wall > waits ? (float)((double)(wall - waits) / 1e6) : 0.0f;
        st->frame_ms[st->n] = (float)((double)f->interval_ns / 1e6);
        st->cpu_ms[st->n] = cpu;
        st->n += 1;
        st->frame_sum += (double)f->interval_ns / 1e6;
        st->cpu_sum += cpu;
        st->update_sum += (double)f->update_ns / 1e6;
        st->render_sum += (double)f->render_ns / 1e6;
        st->acquire_sum += (double)f->acquire_ns / 1e6;
        st->present_sum += (double)f->present_ns / 1e6;
        st->pace_sum += (double)f->pace_ns / 1e6;
        if (f->gpu_ns != 0) {
            st->gpu_sum += (double)f->gpu_ns / 1e6;
            st->gpu_n += 1;
        }
        const br_profiler_scope *scopes = br_profiler_frame_scopes(p, f);
        br_profiler_self_times(scopes, f->scope_count, self);
        for (uint32_t i = 0; i < f->scope_count; i++) {
            st->self_ns[scopes[i].name] += (double)self[i];
            st->total_ns[scopes[i].name] += (double)(scopes[i].end - scopes[i].start);
            st->calls[scopes[i].name] += 1;
        }
    }
    if (st->n == 0) {
        return false;
    }
    SDL_qsort(st->frame_ms, st->n, sizeof(float), br_profiler_float_cmp);
    SDL_qsort(st->cpu_ms, st->n, sizeof(float), br_profiler_float_cmp);
    br_profiler_verdict_input in = p->summary.input; /* refresh / cap as they are now */
    in.interval_ms = (float)(st->frame_sum / st->n);
    in.update_ms = (float)(st->update_sum / st->n);
    in.render_ms = (float)(st->render_sum / st->n);
    in.acquire_ms = (float)(st->acquire_sum / st->n);
    in.present_ms = (float)(st->present_sum / st->n);
    in.pace_ms = (float)(st->pace_sum / st->n);
    in.gpu_ms = st->gpu_n > 0 ? (float)(st->gpu_sum / st->gpu_n) : 0.0f;
    st->verdict = br_profiler_verdict_rule(&in);
    return true;
}

/* Name ids of the `count` scopes with most self time (waits excluded); returns how many. */
static uint32_t br_profiler_top_names(br_profiler_state *p, const br_profiler_stats *st, uint16_t *out, uint32_t count)
{
    uint32_t found = 0;
    while (found < count) {
        int best = -1;
        for (uint32_t id = 1; id < p->name_count; id++) {
            bool wait = id == p->name_acquire || id == p->name_present || id == p->name_pace;
            bool taken = false;
            for (uint32_t j = 0; j < found; j++) {
                taken = taken || out[j] == id;
            }
            if (!wait && !taken && st->calls[id] > 0 && (best < 0 || st->self_ns[id] > st->self_ns[best])) {
                best = (int)id;
            }
        }
        if (best < 0) {
            break;
        }
        out[found++] = (uint16_t)best;
    }
    return found;
}

/* ---- text summary */

static void br_profiler_appendf(char *out, size_t size, const char *fmt, ...)
{
    size_t used = SDL_strlen(out);
    if (used + 1 >= size) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    SDL_vsnprintf(out + used, size - used, fmt, args);
    va_end(args);
}

bool br_profiler_summary(br_engine *e, uint32_t frames, char *out, size_t out_size)
{
    br_profiler_state *p = e->profiler;
    if (out_size > 0) {
        out[0] = '\0';
    }
    br_profiler_stats *st = br_alloc(sizeof(br_profiler_stats));
    if (st == NULL || !br_profiler_stats_compute(e, frames, st)) {
        br_free(st);
        return false;
    }
    double n = (double)st->n;
    br_profiler_appendf(out, out_size, "braty profile: %u frames, driver %s, vsync %s, cap %g\n", st->n,
                        e->gpu != NULL ? br_gpu_driver_name(e->gpu) : "none (headless)", br_vsync_name(e->vsync),
                        (double)e->ticker.max_fps);
    br_profiler_appendf(out, out_size, "frame ms: avg %.3f  p95 %.3f  p99 %.3f  max %.3f  (%.1f FPS)\n",
                        st->frame_sum / n, (double)br_profiler_percentile(st->frame_ms, st->n, 95.0f),
                        (double)br_profiler_percentile(st->frame_ms, st->n, 99.0f), (double)st->frame_ms[st->n - 1],
                        st->frame_sum > 0.0 ? 1000.0 * n / st->frame_sum : 0.0);
    br_profiler_appendf(out, out_size, "cpu ms:   avg %.3f  p95 %.3f  (update %.3f, render %.3f)\n", st->cpu_sum / n,
                        (double)br_profiler_percentile(st->cpu_ms, st->n, 95.0f), st->update_sum / n,
                        st->render_sum / n);
    br_profiler_appendf(out, out_size, "waits ms: acquire %.3f  present %.3f  fps cap %.3f\n", st->acquire_sum / n,
                        st->present_sum / n, st->pace_sum / n);
    if (st->gpu_n > 0) {
        br_profiler_appendf(out, out_size, "gpu ms:   avg %.3f (estimated from fences over %u frames%s)\n",
                            st->gpu_sum / st->gpu_n, st->gpu_n,
                            p->summary.gpu_includes_vsync ? "; vsync on: includes display waits" : "");
    } else {
        br_profiler_appendf(out, out_size, "gpu ms:   unknown (no GPU or no fence finished)\n");
    }
    br_profiler_appendf(out, out_size, "verdict:  %s\n", br_profiler_verdict_name(st->verdict));
    br_profiler_appendf(out, out_size, "top scopes (self ms per frame, total ms per frame, calls per frame):\n");
    uint16_t top[8];
    uint32_t count = br_profiler_top_names(p, st, top, 8);
    for (uint32_t i = 0; i < count; i++) {
        br_profiler_appendf(out, out_size, "  %-32s %8.3f %8.3f %6.1f\n", br_profiler_name(p, top[i]),
                            st->self_ns[top[i]] / 1e6 / n, st->total_ns[top[i]] / 1e6 / n, st->calls[top[i]] / n);
    }
    br_free(st);
    return true;
}

bool br_profiler_summary_json(br_engine *e, uint32_t frames, struct cJSON *out)
{
    br_profiler_state *p = e->profiler;
    br_profiler_stats *st = br_alloc(sizeof(br_profiler_stats));
    if (st == NULL || !br_profiler_stats_compute(e, frames, st)) {
        br_free(st);
        return false;
    }
    double n = (double)st->n;
    cJSON_AddNumberToObject(out, "frames", n);
    cJSON_AddNumberToObject(out, "fps", st->frame_sum > 0.0 ? 1000.0 * n / st->frame_sum : 0.0);
    cJSON_AddNumberToObject(out, "frame_ms_avg", st->frame_sum / n);
    cJSON_AddNumberToObject(out, "frame_ms_p95", br_profiler_percentile(st->frame_ms, st->n, 95.0f));
    cJSON_AddNumberToObject(out, "frame_ms_p99", br_profiler_percentile(st->frame_ms, st->n, 99.0f));
    cJSON_AddNumberToObject(out, "frame_ms_max", st->frame_ms[st->n - 1]);
    cJSON_AddNumberToObject(out, "cpu_ms_avg", st->cpu_sum / n);
    cJSON_AddNumberToObject(out, "cpu_ms_p95", br_profiler_percentile(st->cpu_ms, st->n, 95.0f));
    cJSON_AddNumberToObject(out, "update_ms_avg", st->update_sum / n);
    cJSON_AddNumberToObject(out, "render_ms_avg", st->render_sum / n);
    if (st->gpu_n > 0) {
        cJSON_AddNumberToObject(out, "gpu_ms_avg", st->gpu_sum / st->gpu_n);
    } else {
        cJSON_AddNullToObject(out, "gpu_ms_avg");
    }
    cJSON_AddStringToObject(out, "verdict", br_profiler_verdict_name(st->verdict));
    cJSON *list = cJSON_AddArrayToObject(out, "top_scopes");
    uint16_t top[8];
    uint32_t count = br_profiler_top_names(p, st, top, 8);
    for (uint32_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", br_profiler_name(p, top[i]));
        cJSON_AddNumberToObject(item, "self_ms", st->self_ns[top[i]] / 1e6 / n);
        cJSON_AddNumberToObject(item, "total_ms", st->total_ns[top[i]] / 1e6 / n);
        cJSON_AddNumberToObject(item, "calls", st->calls[top[i]] / n);
        cJSON_AddItemToArray(list, item);
    }
    br_free(st);
    return true;
}

/* ---- Chrome trace-event JSON */

static void br_trace_printf(br_array *buf, const char *fmt, ...)
{
    char line[256];
    va_list args;
    va_start(args, fmt);
    int len = SDL_vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (len > 0) {
        br_array_push_n(buf, line, (uint32_t)(len < (int)sizeof(line) ? len : (int)sizeof(line) - 1));
    }
}

/* A name safe inside a JSON string (scope names are plain identifiers in practice). */
static void br_trace_safe(const char *in, char *out, size_t size)
{
    size_t i = 0;
    for (; in[i] != '\0' && i + 1 < size; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = c == '"' || c == '\\' || c < 0x20 ? '_' : (char)c;
    }
    out[i] = '\0';
}

static double br_trace_us(uint64_t ns, uint64_t t0)
{
    return ns > t0 ? (double)(ns - t0) / 1000.0 : 0.0;
}

static void br_trace_event(br_array *buf, const char *name, char ph, int tid, double ts)
{
    char safe[BR_PROFILER_NAME_SIZE + 16];
    br_trace_safe(name, safe, sizeof(safe));
    br_trace_printf(buf, ",\n{\"name\":\"%s\",\"ph\":\"%c\",\"pid\":1,\"tid\":%d,\"ts\":%.3f}", safe, ph, tid, ts);
}

static void br_trace_frame(br_profiler_state *p, br_array *buf, const br_profiler_frame *f, uint64_t t0)
{
    char label[32];
    SDL_snprintf(label, sizeof(label), "frame %llu", (unsigned long long)f->number);
    br_trace_event(buf, label, 'B', 1, br_trace_us(f->start_ns, t0));
    const br_profiler_scope *scopes = br_profiler_frame_scopes(p, f);
    uint16_t stack[BR_PROFILER_MAX_DEPTH];
    uint32_t depth = 0;
    for (uint32_t i = 0; i < f->scope_count; i++) {
        /* close the open scopes that are not this one's ancestors */
        while (depth > 0 && stack[depth - 1] != scopes[i].parent) {
            uint16_t done = stack[--depth];
            br_trace_event(buf, br_profiler_name(p, scopes[done].name), 'E', 1,
                           br_trace_us(f->start_ns + scopes[done].end, t0));
        }
        br_trace_event(buf, br_profiler_name(p, scopes[i].name), 'B', 1, br_trace_us(f->start_ns + scopes[i].start, t0));
        if (depth < BR_PROFILER_MAX_DEPTH) {
            stack[depth++] = (uint16_t)i;
        }
    }
    while (depth > 0) {
        uint16_t done = stack[--depth];
        br_trace_event(buf, br_profiler_name(p, scopes[done].name), 'E', 1,
                       br_trace_us(f->start_ns + scopes[done].end, t0));
    }
    br_trace_event(buf, label, 'E', 1, br_trace_us(f->end_ns, t0));
    if (f->gpu_ns != 0) {
        br_trace_event(buf, label, 'B', 2, br_trace_us(f->gpu_start_ns, t0));
        br_trace_event(buf, label, 'E', 2, br_trace_us(f->gpu_start_ns + f->gpu_ns, t0));
    }
    double ts = br_trace_us(f->start_ns, t0);
    br_trace_printf(buf, ",\n{\"name\":\"frame ms\",\"ph\":\"C\",\"pid\":1,\"ts\":%.3f,\"args\":{\"interval\":%.3f,\"gpu\":%.3f}}",
                    ts, (double)f->interval_ns / 1e6, (double)f->gpu_ns / 1e6);
    for (int c = 0; c < BR_PROFILER_COUNTER_COUNT; c++) {
        br_trace_printf(buf, ",\n{\"name\":\"%s\",\"ph\":\"C\",\"pid\":1,\"ts\":%.3f,\"args\":{\"value\":%lld}}",
                        br_profiler_counter_name((br_profiler_counter)c), ts, (long long)f->counters[c]);
    }
}

/* Absolute path: relative paths are relative to the working directory, like --screenshot. */
static bool br_profiler_out_path(const char *path, char *out, size_t size)
{
    return br_file_resolve_cwd(path, out, size);
}

bool br_profiler_save(br_engine *e, const char *path)
{
    br_profiler_state *p = e->profiler;
    char full[1024];
    char summary_path[1040];
    if (path == NULL || !br_profiler_out_path(path, full, sizeof(full))) {
        br_log_error("br_profiler_save: bad path");
        return false;
    }
    uint32_t held = br_profiler_frame_count(e);
    if (held == 0) {
        br_log_error("br_profiler_save: no frame recorded");
        return false;
    }
    br_array buf;
    br_array_init(&buf, 1);
    br_trace_printf(&buf, "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n");
    br_trace_printf(&buf, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":1,\"args\":{\"name\":\"CPU (main thread)\"}}");
    br_trace_printf(&buf, ",\n{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":2,\"args\":{\"name\":\"GPU (estimated from fences)\"}}");
    uint64_t t0 = br_profiler_frame_at(p, held - 1)->start_ns;
    for (uint32_t age = held; age > 0; age--) {
        br_trace_frame(p, &buf, br_profiler_frame_at(p, age - 1), t0);
    }
    br_trace_printf(&buf, "\n]}\n");
    bool ok = br_file_write_all(e, full, buf.data, buf.count);
    br_array_free(&buf);
    char text[4096];
    SDL_snprintf(summary_path, sizeof(summary_path), "%s.txt", full);
    if (ok && br_profiler_summary(e, 0, text, sizeof(text))) {
        ok = br_file_write_all(e, summary_path, text, SDL_strlen(text));
        br_log_info("profiler: wrote %s and %s\n%s", full, summary_path, text);
    }
    return ok;
}

void br_profiler_save_at_exit(br_engine *e)
{
    br_profiler_state *p = e->profiler;
    if (p->save_path != NULL) {
        br_profiler_save(e, p->save_path);
    }
}
