/* ws_test_frames.h - RFC 6455 frames written and read by hand for the websocket tests: what a
 * server sends (any bits, any length, unmasked or masked) and a reader of what the client wrote.
 * Independent of wslay, so the tests check the wire format, not the library against itself. */
#ifndef WS_TEST_FRAMES_H
#define WS_TEST_FRAMES_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_TEST_MAX_PAYLOAD 70000

/* One frame read from the client's bytes. */
typedef struct ws_test_frame {
    bool     fin;
    uint8_t  rsv;            /* RSV1..3 as bits 2..0 */
    uint8_t  opcode;
    bool     masked;
    uint8_t  mask[4];
    uint64_t length;
    uint8_t  payload[WS_TEST_MAX_PAYLOAD + 1];   /* unmasked, NUL after */
} ws_test_frame;

/* Writes a frame: first = FIN/RSV/opcode byte (0x81 = final text), the length in its shortest
 * form (length126 / length127 force the longer forms), masked with mask when not NULL. Returns
 * the bytes written. */
static inline size_t ws_test_frame_write_ex(uint8_t *out, uint8_t first, const void *payload, size_t size,
                                            const uint8_t *mask, bool length126, bool length127)
{
    size_t n = 0;
    out[n++] = first;
    uint8_t mask_bit = mask != NULL ? 0x80 : 0;
    if (length127 || size > 65535) {
        out[n++] = mask_bit | 127;
        for (int i = 7; i >= 0; i--) {
            out[n++] = (uint8_t)((uint64_t)size >> (8 * i));
        }
    } else if (length126 || size > 125) {
        out[n++] = mask_bit | 126;
        out[n++] = (uint8_t)(size >> 8);
        out[n++] = (uint8_t)size;
    } else {
        out[n++] = (uint8_t)(mask_bit | size);
    }
    if (mask != NULL) {
        SDL_memcpy(out + n, mask, 4);
        n += 4;
    }
    const uint8_t *p = payload;
    for (size_t i = 0; i < size; i++) {
        out[n + i] = mask != NULL ? (uint8_t)(p[i] ^ mask[i % 4]) : p[i];
    }
    return n + size;
}

/* A server frame: unmasked, shortest length. */
static inline size_t ws_test_frame_write(uint8_t *out, uint8_t first, const void *payload, size_t size)
{
    return ws_test_frame_write_ex(out, first, payload, size, NULL, false, false);
}

/* A close frame from the server: code (0 = no body) and reason. */
static inline size_t ws_test_close_write(uint8_t *out, uint16_t code, const char *reason)
{
    uint8_t body[128];
    size_t size = 0;
    if (code != 0) {
        body[0] = (uint8_t)(code >> 8);
        body[1] = (uint8_t)code;
        size = 2;
        if (reason != NULL) {
            size_t length = SDL_strlen(reason);
            SDL_memcpy(body + 2, reason, length);
            size += length;
        }
    }
    return ws_test_frame_write(out, 0x88, body, size);
}

/* Reads one frame from data: the bytes it took, 0 when the frame is not complete yet. */
static inline size_t ws_test_frame_read(const uint8_t *data, size_t size, ws_test_frame *f)
{
    if (size < 2) {
        return 0;
    }
    f->fin = (data[0] & 0x80) != 0;
    f->rsv = (uint8_t)((data[0] >> 4) & 7);
    f->opcode = data[0] & 0x0F;
    f->masked = (data[1] & 0x80) != 0;
    uint64_t length = data[1] & 0x7F;
    size_t n = 2;
    if (length == 126) {
        if (size < 4) {
            return 0;
        }
        length = ((uint64_t)data[2] << 8) | data[3];
        n = 4;
    } else if (length == 127) {
        if (size < 10) {
            return 0;
        }
        length = 0;
        for (int i = 0; i < 8; i++) {
            length = (length << 8) | data[2 + i];
        }
        n = 10;
    }
    if (f->masked) {
        if (size < n + 4) {
            return 0;
        }
        SDL_memcpy(f->mask, data + n, 4);
        n += 4;
    }
    if (length > WS_TEST_MAX_PAYLOAD || size - n < length) {
        return 0;
    }
    for (size_t i = 0; i < (size_t)length; i++) {
        f->payload[i] = f->masked ? (uint8_t)(data[n + i] ^ f->mask[i % 4]) : data[n + i];
    }
    f->payload[length] = '\0';
    f->length = length;
    return n + (size_t)length;
}

/* The code of a close frame's body (0 when it has none). */
static inline uint16_t ws_test_close_code(const ws_test_frame *f)
{
    return f->length >= 2 ? (uint16_t)((f->payload[0] << 8) | f->payload[1]) : 0;
}

#endif
