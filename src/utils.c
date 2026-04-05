#define _POSIX_C_SOURCE 200809L
/**
 * utils.c — Utility helpers implementation
 */

#include "../include/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Safe memory allocation ──────────────────────────────────────────────── */

void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "FATAL: malloc(%zu) failed — out of memory\n", size);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void *xcalloc(size_t nmemb, size_t size) {
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        fprintf(stderr, "FATAL: calloc(%zu, %zu) failed — out of memory\n",
                nmemb, size);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

char *xstrdup(const char *s) {
    char *copy = strdup(s);
    if (!copy) {
        fprintf(stderr, "FATAL: strdup failed — out of memory\n");
        exit(EXIT_FAILURE);
    }
    return copy;
}

/* ── Debug helpers ───────────────────────────────────────────────────────── */

void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
        if (i < len - 1) printf(" ");
    }
    printf("\n");
}

void hex_to_str(const uint8_t *data, size_t len, char *out) {
    /*
     * Convert each byte to two hex characters.
     * Input: [0xa9, 0x99, 0x3e, ...]
     * Output: "a9993e..."
     */
    for (size_t i = 0; i < len; i++) {
        sprintf(out + (i * 2), "%02x", data[i]);
    }
    out[len * 2] = '\0';
}

/* ── URL encoding ────────────────────────────────────────────────────────── */

void url_encode_bytes(const uint8_t *bytes, size_t len, char *out) {
    /*
     * RFC 3986 "unreserved" characters can be passed through as-is.
     * All other byte values must be percent-encoded as %XX.
     *
     * This is critical for the tracker announce URL: the 20-byte info_hash
     * must be percent-encoded so the tracker can parse the query string.
     *
     * Example: byte 0x89 → "%89"
     *          byte 0x41 → "A"  (unreserved)
     */
    size_t out_pos = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];
        /* Unreserved characters per RFC 3986: A-Z a-z 0-9 - _ . ~ */
        if (isalnum(b) || b == '-' || b == '_' || b == '.' || b == '~') {
            out[out_pos++] = (char)b;
        } else {
            /* Write "%XX" where XX is the hex value of the byte */
            out_pos += sprintf(out + out_pos, "%%%02X", b);
        }
    }
    out[out_pos] = '\0';
}

/* ── Byte order helpers ──────────────────────────────────────────────────── */

uint32_t read_uint32_be(const uint8_t *buf) {
    /*
     * Read 4 bytes in big-endian order into a uint32_t.
     *
     * The BitTorrent peer protocol uses big-endian (network byte order)
     * for all multi-byte integers.
     *
     * x86 is little-endian, so we can't just cast — we must construct
     * the value byte by byte.
     *
     * Example: bytes [0x00, 0x01, 0x86, 0xA0] → 100000 (decimal)
     */
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] <<  8) |
           ((uint32_t)buf[3]);
}

void write_uint32_be(uint8_t *buf, uint32_t value) {
    /*
     * Write a uint32_t in big-endian byte order.
     * The most significant byte goes first.
     *
     * Example: value 100000 → bytes [0x00, 0x01, 0x86, 0xA0]
     */
    buf[0] = (value >> 24) & 0xFF;
    buf[1] = (value >> 16) & 0xFF;
    buf[2] = (value >>  8) & 0xFF;
    buf[3] = (value      ) & 0xFF;
}

uint16_t read_uint16_be(const uint8_t *buf) {
    /*
     * Read 2 bytes in big-endian order.
     * Used for peer port numbers in compact tracker responses.
     *
     * Example: bytes [0x1A, 0xE1] → 6881 (decimal)
     */
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}
