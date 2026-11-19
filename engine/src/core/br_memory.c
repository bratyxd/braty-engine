/* Memory and logging: thin wrappers so every allocation and message goes through SDL. */
#include "braty/br_core.h"

#include <SDL3/SDL.h>
#include <stdarg.h>

void *br_alloc(size_t size)
{
    void *p = SDL_calloc(1, size > 0 ? size : 1);
    if (p == NULL) {
        br_log_error("out of memory allocating %zu bytes", size);
    }
    return p;
}

void *br_realloc(void *ptr, size_t size)
{
    void *p = SDL_realloc(ptr, size > 0 ? size : 1);
    if (p == NULL) {
        br_log_error("out of memory reallocating %zu bytes", size);
    }
    return p;
}

void br_free(void *ptr)
{
    SDL_free(ptr);
}

static void br_log_v(SDL_LogPriority priority, const char *fmt, va_list args)
{
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, priority, fmt, args);
}

void br_log_debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    br_log_v(SDL_LOG_PRIORITY_DEBUG, fmt, args);
    va_end(args);
}

void br_log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    br_log_v(SDL_LOG_PRIORITY_INFO, fmt, args);
    va_end(args);
}

void br_log_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    br_log_v(SDL_LOG_PRIORITY_WARN, fmt, args);
    va_end(args);
}

void br_log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    br_log_v(SDL_LOG_PRIORITY_ERROR, fmt, args);
    va_end(args);
}
