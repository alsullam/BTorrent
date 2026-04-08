/**
 * bencode.h — Bencoding Parser
 *
 * Bencoding is BitTorrent's data serialization format. It supports
 * 4 types: integers, byte strings, lists, and dictionaries.
 *
 * See docs/01_bencoding.md for a full explanation.
 *
 * Usage:
 *   BencodeNode *root = bencode_parse(data, length);
 *   BencodeNode *name = bencode_dict_get(root, "name");
 *   printf("name = %s\n", name->str.data);
 *   bencode_free(root);
 */

#ifndef BTORRENT_BENCODE_H
#define BTORRENT_BENCODE_H

#include <stddef.h>
#include <stdint.h>

/* Node types */

typedef enum {
    BENCODE_INT,    /* i42e  → integer */
    BENCODE_STR,    /* 4:spam → byte string */
    BENCODE_LIST,   /* l...e  → list */
    BENCODE_DICT    /* d...e  → dictionary (sorted string keys) */
} BencodeType;

/* The node struct */

typedef struct BencodeNode BencodeNode;

struct BencodeNode {
    BencodeType type;

    union {
        /* BENCODE_INT */
        long long integer;

        /* BENCODE_STR */
        struct {
            uint8_t *data;  /* raw bytes (NOT null-terminated!) */
            size_t   len;   /* byte count */
        } str;

        /* BENCODE_LIST */
        struct {
            BencodeNode **items;  /* array of child nodes */
            size_t        count;  /* number of items */
        } list;

        /* BENCODE_DICT */
        struct {
            char        **keys;   /* sorted string keys (null-terminated) */
            BencodeNode **vals;   /* corresponding values */
            size_t        count;  /* number of key-value pairs */
        } dict;
    };
};

/* Parse / free */

/**
 * bencode_parse - parse a bencoded buffer.
 *
 * @data   raw bencoded bytes (e.g. contents of a .torrent file)
 * @len    length of data
 * @return root BencodeNode, or NULL on parse error
 *
 * The returned tree must be freed with bencode_free().
 *
 * IMPORTANT: The string nodes point into `data` (no extra copy is made).
 * Keep `data` alive as long as you use the tree!
 */
BencodeNode *bencode_parse(const uint8_t *data, size_t len);

/**
 * bencode_free - recursively free a BencodeNode tree.
 */
void bencode_free(BencodeNode *node);

/* Dictionary lookup */

/**
 * bencode_dict_get - look up a key in a BENCODE_DICT node.
 *
 * @node  must be type BENCODE_DICT
 * @key   null-terminated string key to look for
 * @return the value node, or NULL if key not found
 *
 * Keys are stored in sorted order, but we do a simple linear scan here
 * since dicts in .torrent files are small.
 */
BencodeNode *bencode_dict_get(const BencodeNode *node, const char *key);

/* Debug helpers */

/**
 * bencode_print - pretty-print a BencodeNode tree to stdout.
 * Great for exploring .torrent file structure.
 *
 * @node   root to print
 * @indent current indentation level (start with 0)
 */
void bencode_print(const BencodeNode *node, int indent);

/* Internal: parser state */

/**
 * BencodeParser holds the cursor position while parsing.
 * You don't use this directly — bencode_parse() handles it.
 */
typedef struct {
    const uint8_t *data;  /* start of buffer */
    size_t         len;   /* total length */
    size_t         pos;   /* current read position */
} BencodeParser;

#endif /* BTORRENT_BENCODE_H */
