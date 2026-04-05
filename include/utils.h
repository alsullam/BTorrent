/**
 * utils.h — Utility helpers
 *
 * Small helper functions used across the codebase:
 *   - Memory allocation with panic-on-failure
 *   - Hex printing for debugging
 *   - URL encoding for tracker requests
 */

#ifndef BTORRENT_UTILS_H
#define BTORRENT_UTILS_H

#include <stdint.h>
#include <stddef.h>

/* ── Safe memory allocation ──────────────────────────────────────────────── */

/**
 * xmalloc - malloc that exits on failure.
 * We never check NULL returns — if the system is out of memory,
 * there's nothing sensible we can do anyway.
 */
void *xmalloc(size_t size);

/**
 * xcalloc - calloc that exits on failure (zero-initializes memory).
 */
void *xcalloc(size_t nmemb, size_t size);

/**
 * xstrdup - strdup that exits on failure.
 */
char *xstrdup(const char *s);

/* ── Debug helpers ───────────────────────────────────────────────────────── */

/**
 * print_hex - prints `len` bytes from `data` as hex.
 * Example output: "a9 99 3e 36 47 06 81 6a ba 3e ..."
 *
 * Useful for inspecting info_hash, piece hashes, peer IDs.
 */
void print_hex(const uint8_t *data, size_t len);

/**
 * hex_to_str - converts `len` bytes of `data` into a null-terminated
 * lowercase hex string in `out`. `out` must be at least `len*2 + 1` bytes.
 */
void hex_to_str(const uint8_t *data, size_t len, char *out);

/* ── URL encoding ────────────────────────────────────────────────────────── */

/**
 * url_encode_bytes - percent-encodes raw bytes for use in a URL.
 *
 * Unreserved characters (letters, digits, -, _, ., ~) are passed through.
 * All other bytes become %XX.
 *
 * `out` must have room for at least `len * 3 + 1` bytes (worst case).
 *
 * Used to URL-encode the 20-byte info_hash in tracker requests.
 */
void url_encode_bytes(const uint8_t *bytes, size_t len, char *out);

/* ── Byte order helpers ──────────────────────────────────────────────────── */

/**
 * read_uint32_be - reads a 32-bit big-endian integer from `buf`.
 * Used for parsing peer wire protocol messages (all ints are big-endian).
 */
uint32_t read_uint32_be(const uint8_t *buf);

/**
 * write_uint32_be - writes a 32-bit integer to `buf` in big-endian order.
 */
void write_uint32_be(uint8_t *buf, uint32_t value);

/**
 * read_uint16_be - reads a 16-bit big-endian integer from `buf`.
 * Used for parsing peer port numbers from compact tracker responses.
 */
uint16_t read_uint16_be(const uint8_t *buf);

#endif /* BTORRENT_UTILS_H */
