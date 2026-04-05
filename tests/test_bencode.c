/**
 * test_bencode.c — Unit Tests for the Bencoding Parser
 *
 * Tests cover all 4 types (int, string, list, dict), edge cases,
 * and the nested/mixed structures found in real .torrent files.
 *
 * Run with: make test
 *
 * Each test prints PASS or FAIL. Exit code is 0 if all tests pass.
 */

#include "../include/bencode.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Test infrastructure ─────────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg)                                           \
    do {                                                            \
        tests_run++;                                                \
        if (cond) {                                                 \
            tests_passed++;                                         \
            printf("  PASS  %s\n", msg);                           \
        } else {                                                    \
            tests_failed++;                                         \
            printf("  FAIL  %s (line %d)\n", msg, __LINE__);       \
        }                                                           \
    } while (0)

/* Helper to parse a C string literal (wraps the uint8_t cast) */
static BencodeNode *parse_str(const char *s) {
    return bencode_parse((const uint8_t *)s, strlen(s));
}

/* ── Tests: Integer ──────────────────────────────────────────────────────── */

static void test_integers(void) {
    printf("\n--- Integer tests ---\n");

    BencodeNode *n;

    /* Basic positive integer */
    n = parse_str("i42e");
    ASSERT(n != NULL, "parse i42e");
    ASSERT(n->type == BENCODE_INT, "i42e is INT type");
    ASSERT(n->integer == 42, "i42e == 42");
    bencode_free(n);

    /* Zero */
    n = parse_str("i0e");
    ASSERT(n != NULL, "parse i0e");
    ASSERT(n->integer == 0, "i0e == 0");
    bencode_free(n);

    /* Negative integer */
    n = parse_str("i-7e");
    ASSERT(n != NULL, "parse i-7e");
    ASSERT(n->integer == -7, "i-7e == -7");
    bencode_free(n);

    /* Large integer */
    n = parse_str("i1234567890e");
    ASSERT(n != NULL, "parse large integer");
    ASSERT(n->integer == 1234567890LL, "large integer value");
    bencode_free(n);
}

/* ── Tests: String ───────────────────────────────────────────────────────── */

static void test_strings(void) {
    printf("\n--- String tests ---\n");

    BencodeNode *n;

    /* Basic string */
    n = parse_str("4:spam");
    ASSERT(n != NULL, "parse 4:spam");
    ASSERT(n->type == BENCODE_STR, "4:spam is STR type");
    ASSERT(n->str.len == 4, "4:spam length == 4");
    ASSERT(memcmp(n->str.data, "spam", 4) == 0, "4:spam data == 'spam'");
    bencode_free(n);

    /* Empty string */
    n = parse_str("0:");
    ASSERT(n != NULL, "parse 0:");
    ASSERT(n->str.len == 0, "0: length == 0");
    bencode_free(n);

    /* String with spaces */
    n = parse_str("11:hello world");
    ASSERT(n != NULL, "parse 11:hello world");
    ASSERT(n->str.len == 11, "11:hello world length == 11");
    ASSERT(memcmp(n->str.data, "hello world", 11) == 0,
           "11:hello world data correct");
    bencode_free(n);

    /* Binary data (non-printable) */
    const uint8_t raw[] = { '2', ':', 0x00, 0xFF };
    n = bencode_parse(raw, sizeof(raw));
    ASSERT(n != NULL, "parse binary string");
    ASSERT(n->str.len == 2, "binary string length == 2");
    ASSERT(n->str.data[0] == 0x00, "binary string byte 0 == 0x00");
    ASSERT(n->str.data[1] == 0xFF, "binary string byte 1 == 0xFF");
    bencode_free(n);
}

/* ── Tests: List ─────────────────────────────────────────────────────────── */

static void test_lists(void) {
    printf("\n--- List tests ---\n");

    BencodeNode *n;

    /* List of two strings */
    n = parse_str("l4:spam3:fooe");
    ASSERT(n != NULL, "parse l4:spam3:fooe");
    ASSERT(n->type == BENCODE_LIST, "is LIST type");
    ASSERT(n->list.count == 2, "list has 2 items");
    ASSERT(n->list.items[0]->type == BENCODE_STR, "item[0] is STR");
    ASSERT(memcmp(n->list.items[0]->str.data, "spam", 4) == 0,
           "item[0] == 'spam'");
    ASSERT(memcmp(n->list.items[1]->str.data, "foo", 3) == 0,
           "item[1] == 'foo'");
    bencode_free(n);

    /* Mixed list: string + integer */
    n = parse_str("l4:spami42ee");
    ASSERT(n != NULL, "parse mixed list");
    ASSERT(n->list.count == 2, "mixed list has 2 items");
    ASSERT(n->list.items[0]->type == BENCODE_STR, "item[0] is STR");
    ASSERT(n->list.items[1]->type == BENCODE_INT, "item[1] is INT");
    ASSERT(n->list.items[1]->integer == 42, "item[1] == 42");
    bencode_free(n);

    /* Empty list */
    n = parse_str("le");
    ASSERT(n != NULL, "parse empty list");
    ASSERT(n->list.count == 0, "empty list has 0 items");
    bencode_free(n);

    /* Nested list */
    n = parse_str("lli1ei2eeli3ei4eee");
    ASSERT(n != NULL, "parse nested list");
    ASSERT(n->list.count == 2, "outer list has 2 items");
    ASSERT(n->list.items[0]->type == BENCODE_LIST, "item[0] is LIST");
    ASSERT(n->list.items[0]->list.count == 2, "inner list[0] has 2 items");
    ASSERT(n->list.items[0]->list.items[0]->integer == 1,
           "inner[0][0] == 1");
    ASSERT(n->list.items[1]->list.items[1]->integer == 4,
           "inner[1][1] == 4");
    bencode_free(n);
}

/* ── Tests: Dictionary ───────────────────────────────────────────────────── */

static void test_dicts(void) {
    printf("\n--- Dictionary tests ---\n");

    BencodeNode *n, *val;

    /* Simple dict */
    n = parse_str("d3:cow3:moo4:spam4:eggse");
    ASSERT(n != NULL, "parse simple dict");
    ASSERT(n->type == BENCODE_DICT, "is DICT type");
    ASSERT(n->dict.count == 2, "dict has 2 keys");

    val = bencode_dict_get(n, "cow");
    ASSERT(val != NULL, "dict_get 'cow' found");
    ASSERT(val->type == BENCODE_STR, "'cow' value is STR");
    ASSERT(memcmp(val->str.data, "moo", 3) == 0, "'cow' == 'moo'");

    val = bencode_dict_get(n, "spam");
    ASSERT(val != NULL, "dict_get 'spam' found");
    ASSERT(memcmp(val->str.data, "eggs", 4) == 0, "'spam' == 'eggs'");

    val = bencode_dict_get(n, "nothere");
    ASSERT(val == NULL, "dict_get missing key returns NULL");

    bencode_free(n);

    /* Dict with integer value */
    n = parse_str("d6:lengthi12345ee");
    ASSERT(n != NULL, "parse dict with int value");
    val = bencode_dict_get(n, "length");
    ASSERT(val != NULL, "dict_get 'length' found");
    ASSERT(val->type == BENCODE_INT, "'length' is INT");
    ASSERT(val->integer == 12345, "'length' == 12345");
    bencode_free(n);

    /* Nested dict */
    n = parse_str("d4:infod4:name5:testsee");
    ASSERT(n != NULL, "parse nested dict");
    val = bencode_dict_get(n, "info");
    ASSERT(val != NULL, "'info' key found");
    ASSERT(val->type == BENCODE_DICT, "'info' is DICT");
    BencodeNode *name = bencode_dict_get(val, "name");
    ASSERT(name != NULL, "'info.name' found");
    ASSERT(memcmp(name->str.data, "tests", 5) == 0, "'info.name' == 'tests'");
    bencode_free(n);
}

/* ── Tests: Real .torrent-like structure ─────────────────────────────────── */

static void test_torrent_like(void) {
    printf("\n--- Torrent-like structure tests ---\n");

    /*
     * This simulates the structure of a real single-file .torrent:
     *
     * {
     *   "announce": "http://tracker.example.com/announce",
     *   "info": {
     *     "length": 1048576,
     *     "name": "test.iso",
     *     "piece length": 262144,
     *     "pieces": <20 bytes>   (one piece hash)
     *   }
     * }
     *
     * We use a made-up 20-byte hash: 20 0xAB bytes for simplicity.
     */

    /* Build the bencode by hand */
    const char *pre =
        "d"
          "8:announce"
          "35:http://tracker.example.com/announce"
          "4:info"
          "d"
            "6:lengthi1048576e"
            "4:name8:test.iso"
            "12:piece lengthi262144e"
            "6:pieces20:";

    const char *post = "ee";

    uint8_t hash_bytes[20];
    memset(hash_bytes, 0xAB, 20);

    size_t total_len = strlen(pre) + 20 + strlen(post);
    uint8_t *buf = xmalloc(total_len);
    memcpy(buf, pre, strlen(pre));
    memcpy(buf + strlen(pre), hash_bytes, 20);
    memcpy(buf + strlen(pre) + 20, post, strlen(post));

    BencodeNode *root = bencode_parse(buf, total_len);
    ASSERT(root != NULL, "parse torrent-like structure");
    ASSERT(root->type == BENCODE_DICT, "root is DICT");

    BencodeNode *announce = bencode_dict_get(root, "announce");
    ASSERT(announce != NULL, "announce key found");
    ASSERT(announce->type == BENCODE_STR, "announce is STR");

    BencodeNode *info = bencode_dict_get(root, "info");
    ASSERT(info != NULL, "info key found");
    ASSERT(info->type == BENCODE_DICT, "info is DICT");

    BencodeNode *length = bencode_dict_get(info, "length");
    ASSERT(length != NULL, "info.length found");
    ASSERT(length->integer == 1048576, "info.length == 1048576");

    BencodeNode *piece_length = bencode_dict_get(info, "piece length");
    ASSERT(piece_length != NULL, "info.piece_length found");
    ASSERT(piece_length->integer == 262144, "piece length == 262144");

    BencodeNode *pieces = bencode_dict_get(info, "pieces");
    ASSERT(pieces != NULL, "info.pieces found");
    ASSERT(pieces->str.len == 20, "pieces is 20 bytes");
    ASSERT(pieces->str.data[0] == 0xAB, "pieces hash byte correct");

    bencode_free(root);
    free(buf);
}

/* ── Tests: Edge cases ───────────────────────────────────────────────────── */

static void test_edge_cases(void) {
    printf("\n--- Edge case tests ---\n");

    /* Empty input */
    BencodeNode *n = bencode_parse((const uint8_t *)"", 0);
    ASSERT(n == NULL, "empty input returns NULL");

    /* URL-encoding test via utils */
    uint8_t info_hash[20];
    memset(info_hash, 0, 20);
    info_hash[0] = 0x89;
    info_hash[1] = 0xAB;
    info_hash[2] = 0x41; /* 'A' — unreserved, should not be encoded */

    char encoded[64];
    url_encode_bytes(info_hash, 3, encoded);
    ASSERT(strcmp(encoded, "%89%ABA") == 0,
           "url_encode: 0x89=%%89, 0xAB=%%AB, 0x41=A");

    /* Big-endian read/write */
    uint8_t buf[4] = { 0x00, 0x01, 0x86, 0xA0 };
    uint32_t val = read_uint32_be(buf);
    ASSERT(val == 100000, "read_uint32_be(00 01 86 A0) == 100000");

    uint8_t out[4];
    write_uint32_be(out, 100000);
    ASSERT(out[0] == 0x00 && out[1] == 0x01 &&
           out[2] == 0x86 && out[3] == 0xA0,
           "write_uint32_be(100000) == 00 01 86 A0");

    /* 16-bit big-endian */
    uint8_t port_bytes[2] = { 0x1A, 0xE1 };
    uint16_t port = read_uint16_be(port_bytes);
    ASSERT(port == 6881, "read_uint16_be(1A E1) == 6881");
}

/* ── SHA-1 test vectors ──────────────────────────────────────────────────── */

static void test_sha1(void) {
    printf("\n--- SHA-1 test vectors ---\n");

    /*
     * These are the official RFC 3174 test vectors.
     * If your SHA-1 implementation is correct, these must pass.
     */
    #include "sha1.h"

    struct { const char *input; const char *expected_hex; } vectors[] = {
        { "abc",
          "a9993e364706816aba3e25717850c26c9cd0d89d" },
        { "",
          "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
        { "The quick brown fox jumps over the lazy dog",
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12" },
    };

    for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); i++) {
        uint8_t hash[20];
        sha1((const uint8_t *)vectors[i].input, strlen(vectors[i].input), hash);

        char hex[41];
        hex_to_str(hash, 20, hex);

        char msg[128];
        snprintf(msg, sizeof(msg), "SHA-1(\"%s\")", vectors[i].input);
        ASSERT(strcmp(hex, vectors[i].expected_hex) == 0, msg);
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("BTorrent Unit Tests\n");
    printf("===================\n");

    test_integers();
    test_strings();
    test_lists();
    test_dicts();
    test_torrent_like();
    test_edge_cases();
    test_sha1();

    printf("\n===================\n");
    printf("Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf("\n");

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
