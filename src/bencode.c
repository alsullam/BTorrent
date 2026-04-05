/**
 * bencode.c — Bencoding Parser Implementation
 *
 * Recursive descent parser for the bencoding format.
 * See docs/01_bencoding.md for a full explanation.
 */

#include "../include/bencode.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Forward declarations (for mutual recursion) ─────────────────────────── */

static BencodeNode *parse_value(BencodeParser *p);
static BencodeNode *parse_integer(BencodeParser *p);
static BencodeNode *parse_string(BencodeParser *p);
static BencodeNode *parse_list(BencodeParser *p);
static BencodeNode *parse_dict(BencodeParser *p);

/* ── Helper: peek at current byte without advancing ─────────────────────── */

static int peek(BencodeParser *p) {
    if (p->pos >= p->len) return -1;  /* EOF sentinel */
    return p->data[p->pos];
}

/* ── Helper: consume current byte and advance ────────────────────────────── */

static int consume(BencodeParser *p) {
    if (p->pos >= p->len) return -1;
    return p->data[p->pos++];
}

/* ── Helper: expect a specific byte (and consume it) ────────────────────── */

static int expect(BencodeParser *p, char expected) {
    int c = consume(p);
    if (c != expected) {
        fprintf(stderr, "bencode: expected '%c' at pos %zu, got '%c'\n",
                expected, p->pos - 1, (char)c);
        return -1;
    }
    return 0;
}

/* ── Helper: allocate a new node ─────────────────────────────────────────── */

static BencodeNode *new_node(BencodeType type) {
    BencodeNode *n = xcalloc(1, sizeof(BencodeNode));
    n->type = type;
    return n;
}

/* ── parse_integer ───────────────────────────────────────────────────────── */

static BencodeNode *parse_integer(BencodeParser *p) {
    /*
     * Format: i<decimal number>e
     *
     * We consume the 'i', then read digits (and optional leading '-'),
     * then consume the 'e'.
     *
     * We use strtoll to handle the numeric conversion.
     */

    if (expect(p, 'i') < 0) return NULL;

    /* Collect digits (and optional leading '-') into a temp buffer */
    char buf[32];
    int  buf_len = 0;

    /* Handle negative numbers */
    if (peek(p) == '-') {
        buf[buf_len++] = (char)consume(p);
    }

    /* Read decimal digits */
    while (isdigit(peek(p))) {
        if (buf_len >= 30) {
            fprintf(stderr, "bencode: integer too large\n");
            return NULL;
        }
        buf[buf_len++] = (char)consume(p);
    }
    buf[buf_len] = '\0';

    if (buf_len == 0 || (buf_len == 1 && buf[0] == '-')) {
        fprintf(stderr, "bencode: empty integer\n");
        return NULL;
    }

    if (expect(p, 'e') < 0) return NULL;

    BencodeNode *node = new_node(BENCODE_INT);
    node->integer = strtoll(buf, NULL, 10);
    return node;
}

/* ── parse_string ────────────────────────────────────────────────────────── */

static BencodeNode *parse_string(BencodeParser *p) {
    /*
     * Format: <length>:<bytes>
     *
     * Read the decimal length, consume ':', then point directly into
     * the data buffer (no copy!) for the string bytes.
     *
     * Strings in bencoding are raw BYTE strings, not necessarily UTF-8.
     * Piece hashes are 20-byte binary strings, not text!
     */

    /* Read decimal length */
    char len_buf[20];
    int  len_buf_pos = 0;

    while (isdigit(peek(p))) {
        if (len_buf_pos >= 18) {
            fprintf(stderr, "bencode: string length too large\n");
            return NULL;
        }
        len_buf[len_buf_pos++] = (char)consume(p);
    }
    len_buf[len_buf_pos] = '\0';

    if (len_buf_pos == 0) {
        fprintf(stderr, "bencode: expected string length at pos %zu\n", p->pos);
        return NULL;
    }

    size_t str_len = (size_t)atol(len_buf);

    if (expect(p, ':') < 0) return NULL;

    /* Verify enough bytes remain */
    if (p->pos + str_len > p->len) {
        fprintf(stderr, "bencode: string length %zu exceeds buffer\n", str_len);
        return NULL;
    }

    BencodeNode *node = new_node(BENCODE_STR);
    node->str.data = (uint8_t *)(p->data + p->pos); /* point into buffer */
    node->str.len  = str_len;
    p->pos += str_len;  /* advance past the string data */

    return node;
}

/* ── parse_list ──────────────────────────────────────────────────────────── */

static BencodeNode *parse_list(BencodeParser *p) {
    /*
     * Format: l<item1><item2>...e
     *
     * We don't know the count in advance, so we use a dynamic array:
     * start with capacity 8, double when full.
     */

    if (expect(p, 'l') < 0) return NULL;

    BencodeNode *node = new_node(BENCODE_LIST);

    size_t capacity = 8;
    node->list.items = xmalloc(capacity * sizeof(BencodeNode *));
    node->list.count = 0;

    /* Parse items until we hit 'e' (end of list) */
    while (peek(p) != 'e' && peek(p) != -1) {
        BencodeNode *item = parse_value(p);
        if (!item) {
            bencode_free(node);
            return NULL;
        }

        /* Grow array if needed */
        if (node->list.count >= capacity) {
            capacity *= 2;
            node->list.items = realloc(node->list.items,
                                       capacity * sizeof(BencodeNode *));
        }

        node->list.items[node->list.count++] = item;
    }

    if (expect(p, 'e') < 0) {
        bencode_free(node);
        return NULL;
    }

    return node;
}

/* ── parse_dict ──────────────────────────────────────────────────────────── */

static BencodeNode *parse_dict(BencodeParser *p) {
    /*
     * Format: d<key1><value1><key2><value2>...e
     *
     * Keys must be strings and must be in lexicographic order (required by spec).
     * We store them as null-terminated C strings (copied from the buffer).
     */

    if (expect(p, 'd') < 0) return NULL;

    BencodeNode *node = new_node(BENCODE_DICT);

    size_t capacity = 8;
    node->dict.keys  = xmalloc(capacity * sizeof(char *));
    node->dict.vals  = xmalloc(capacity * sizeof(BencodeNode *));
    node->dict.count = 0;

    while (peek(p) != 'e' && peek(p) != -1) {
        /* Keys must be byte strings */
        if (!isdigit(peek(p))) {
            fprintf(stderr, "bencode: dict key must be a string at pos %zu\n",
                    p->pos);
            bencode_free(node);
            return NULL;
        }

        /* Parse key (a string) */
        BencodeNode *key_node = parse_string(p);
        if (!key_node) {
            bencode_free(node);
            return NULL;
        }

        /* Copy key to a null-terminated C string (keys need \0 for strcmp) */
        char *key = xmalloc(key_node->str.len + 1);
        memcpy(key, key_node->str.data, key_node->str.len);
        key[key_node->str.len] = '\0';
        /* The key_node itself is a "view" into the buffer — we free it now
         * since we copied the string out. The data pointer is not freed
         * (it's part of the input buffer). */
        free(key_node);

        /* Parse value (any bencode type) */
        BencodeNode *val = parse_value(p);
        if (!val) {
            free(key);
            bencode_free(node);
            return NULL;
        }

        /* Grow arrays if needed */
        if (node->dict.count >= capacity) {
            capacity *= 2;
            node->dict.keys = realloc(node->dict.keys,
                                      capacity * sizeof(char *));
            node->dict.vals = realloc(node->dict.vals,
                                      capacity * sizeof(BencodeNode *));
        }

        node->dict.keys[node->dict.count] = key;
        node->dict.vals[node->dict.count] = val;
        node->dict.count++;
    }

    if (expect(p, 'e') < 0) {
        bencode_free(node);
        return NULL;
    }

    return node;
}

/* ── parse_value — the dispatcher ───────────────────────────────────────── */

static BencodeNode *parse_value(BencodeParser *p) {
    /*
     * Look at the first character to determine the type:
     *   'i' → integer
     *   'l' → list
     *   'd' → dictionary
     *   digit → string (the digit is the start of the length)
     */
    int c = peek(p);
    if (c == -1) {
        fprintf(stderr, "bencode: unexpected end of data\n");
        return NULL;
    }

    if (c == 'i') return parse_integer(p);
    if (c == 'l') return parse_list(p);
    if (c == 'd') return parse_dict(p);
    if (isdigit(c)) return parse_string(p);

    fprintf(stderr, "bencode: unexpected character '%c' at pos %zu\n",
            (char)c, p->pos);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

BencodeNode *bencode_parse(const uint8_t *data, size_t len) {
    BencodeParser p = { .data = data, .len = len, .pos = 0 };
    return parse_value(&p);
}

void bencode_free(BencodeNode *node) {
    if (!node) return;

    switch (node->type) {
        case BENCODE_INT:
        case BENCODE_STR:
            /* String data points into the input buffer — don't free it! */
            break;

        case BENCODE_LIST:
            for (size_t i = 0; i < node->list.count; i++) {
                bencode_free(node->list.items[i]);
            }
            free(node->list.items);
            break;

        case BENCODE_DICT:
            for (size_t i = 0; i < node->dict.count; i++) {
                free(node->dict.keys[i]);      /* keys are copied strings */
                bencode_free(node->dict.vals[i]);
            }
            free(node->dict.keys);
            free(node->dict.vals);
            break;
    }

    free(node);
}

BencodeNode *bencode_dict_get(const BencodeNode *node, const char *key) {
    if (!node || node->type != BENCODE_DICT) return NULL;

    /* Linear scan — dicts in .torrent files have ~10 keys at most */
    for (size_t i = 0; i < node->dict.count; i++) {
        if (strcmp(node->dict.keys[i], key) == 0) {
            return node->dict.vals[i];
        }
    }
    return NULL;
}

void bencode_print(const BencodeNode *node, int indent) {
    if (!node) {
        printf("(null)\n");
        return;
    }

    /* Print `indent` spaces for visual nesting */
    for (int i = 0; i < indent; i++) printf("  ");

    switch (node->type) {
        case BENCODE_INT:
            printf("INT: %lld\n", node->integer);
            break;

        case BENCODE_STR:
            /* Print printable strings as text, binary as hex */
            printf("STR(%zu): ", node->str.len);
            {
                int printable = 1;
                for (size_t i = 0; i < node->str.len && i < 64; i++) {
                    if (!isprint(node->str.data[i]) &&
                        !isspace(node->str.data[i])) {
                        printable = 0;
                        break;
                    }
                }
                if (printable && node->str.len < 200) {
                    printf("\"%.*s\"", (int)node->str.len, node->str.data);
                } else {
                    printf("<binary, %zu bytes, first bytes: ", node->str.len);
                    for (size_t i = 0; i < node->str.len && i < 8; i++) {
                        printf("%02x ", node->str.data[i]);
                    }
                    printf("...>");
                }
            }
            printf("\n");
            break;

        case BENCODE_LIST:
            printf("LIST (%zu items):\n", node->list.count);
            for (size_t i = 0; i < node->list.count; i++) {
                bencode_print(node->list.items[i], indent + 1);
            }
            break;

        case BENCODE_DICT:
            printf("DICT (%zu keys):\n", node->dict.count);
            for (size_t i = 0; i < node->dict.count; i++) {
                for (int j = 0; j < indent + 1; j++) printf("  ");
                printf("[%s] =>\n", node->dict.keys[i]);
                bencode_print(node->dict.vals[i], indent + 2);
            }
            break;
    }
}
