/* Small string and path helpers. */
#include "braty/br_core.h"

#include <SDL3/SDL.h>

char *br_strdup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t len = SDL_strlen(s);
    char *copy = br_alloc(len + 1);
    if (copy != NULL) {
        SDL_memcpy(copy, s, len + 1);
    }
    return copy;
}

bool br_str_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return SDL_strcmp(a, b) == 0;
}

bool br_str_starts_with(const char *s, const char *prefix)
{
    return SDL_strncmp(s, prefix, SDL_strlen(prefix)) == 0;
}

bool br_str_ends_with(const char *s, const char *suffix)
{
    size_t len = SDL_strlen(s);
    size_t suffix_len = SDL_strlen(suffix);
    if (suffix_len > len) {
        return false;
    }
    return SDL_strcmp(s + len - suffix_len, suffix) == 0;
}

bool br_str_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return false;
    }
    size_t needed = SDL_strlcpy(dst, src, dst_size);
    return needed < dst_size;
}

uint32_t br_str_hash(const char *s)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

bool br_path_join(char *dst, size_t dst_size, const char *a, const char *b)
{
    if (!br_str_copy(dst, dst_size, a)) {
        return false;
    }
    size_t len = SDL_strlen(dst);
    bool a_has_slash = len > 0 && (dst[len - 1] == '/' || dst[len - 1] == '\\');
    if (len > 0 && !a_has_slash) {
        if (SDL_strlcat(dst, "/", dst_size) >= dst_size) {
            return false;
        }
    }
    while (*b == '/') {
        b++;
    }
    return SDL_strlcat(dst, b, dst_size) < dst_size;
}

const char *br_path_extension(const char *path)
{
    const char *dot = NULL;
    for (const char *p = path; *p != 0; p++) {
        if (*p == '.') {
            dot = p;
        } else if (*p == '/' || *p == '\\') {
            dot = NULL;
        }
    }
    return dot != NULL ? dot + 1 : path + SDL_strlen(path);
}

bool br_path_is_absolute(const char *path)
{
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    /* drive letter "C:" - harmless on platforms that never produce it */
    bool letter = (path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z');
    return letter && path[1] == ':';
}
