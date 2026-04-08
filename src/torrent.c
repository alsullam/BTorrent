/**
 * torrent.c — .torrent File Parser
 *
 * Reads a .torrent file from disk, bencodes-parses it, extracts all
 * metadata into a TorrentInfo struct, and computes the info_hash.
 *
 * See docs/02_torrent_file.md for a full explanation.
 */

#include "../include/torrent.h"
#include "../include/bencode.h"
#include "sha1.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper: read entire file into memory */

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }

    /* Seek to end to get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fprintf(stderr, "torrent: file is empty: %s\n", path);
        fclose(f);
        return NULL;
    }

    uint8_t *buf = xmalloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "torrent: failed to read file: %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_len = (size_t)size;
    return buf;
}

/* Helper: compute info_hash */

/*
 * The info_hash is SHA-1 of the raw bencoded bytes of the "info" dictionary.
 *
 * The tricky part: we need the EXACT bytes as they appear in the file,
 * not a re-encoding. So we locate the start of "info" in the raw buffer
 * and find where it ends, then hash those bytes directly.
 *
 * To find the info dict boundaries, we use a second parser that tracks
 * positions. We scan for the key "4:info" in the raw bytes, note the
 * offset after the key, parse the value to find where it ends, and
 * hash that range.
 */
static int compute_info_hash(const uint8_t *raw, size_t raw_len,
                              uint8_t *info_hash) {
    /*
     * Strategy: scan for "4:info" in the raw bytes, then find where the
     * value (a dict starting with 'd') begins and ends.
     *
     * We use a simple BencodeParser to parse the top-level dict and find
     * the info dict's start/end positions.
     */

    /* Find "4:info" as raw bytes */
    const uint8_t needle[] = "4:info";
    size_t needle_len = 6;

    for (size_t i = 0; i + needle_len < raw_len; i++) {
        if (memcmp(raw + i, needle, needle_len) == 0) {
            /* Found "4:info" — the value starts right after */
            size_t info_start = i + needle_len;

            /* Now we need to find where the info value ends.
             * We'll parse a bencode value from info_start and track how
             * many bytes it consumed. */
            BencodeParser p = {
                .data = raw,
                .len  = raw_len,
                .pos  = info_start
            };

            /* Parse the info value just to advance past it */
            BencodeNode *info_node = bencode_parse(raw + info_start,
                                                    raw_len - info_start);
            if (!info_node) {
                fprintf(stderr, "torrent: failed to parse info dict\n");
                return -1;
            }

            /*
             * To find the end position we need to re-parse with position
             * tracking. Here we use a simpler approach: re-parse the whole
             * file and ask the parser for the end position of info.
             *
             * Since we can't easily get the end position from bencode_parse(),
             * we parse incrementally with our own parser state.
             */
            (void)p; /* suppress unused warning — see note below */

            /*
             * SIMPLER APPROACH:
             * We know the info value starts at `info_start` with character 'd'.
             * We'll walk forward counting nested 'd'/'l'....'e' pairs to
             * find where the info dict ends.
             */
            size_t depth = 0;
            size_t pos   = info_start;
            int    in_string = 0;

            while (pos < raw_len) {
                if (in_string > 0) {
                    pos++;
                    in_string--;
                    continue;
                }

                uint8_t c = raw[pos];

                if (c == 'd' || c == 'l') {
                    depth++;
                    pos++;
                } else if (c == 'e') {
                    depth--;
                    pos++;
                    if (depth == 0) break;
                } else if (c == 'i') {
                    /* integer: skip to matching 'e' */
                    while (pos < raw_len && raw[pos] != 'e') pos++;
                    pos++; /* consume 'e' */
                } else if (c >= '0' && c <= '9') {
                    /* string: read length, skip data */
                    size_t slen = 0;
                    while (pos < raw_len && raw[pos] >= '0' && raw[pos] <= '9') {
                        slen = slen * 10 + (raw[pos] - '0');
                        pos++;
                    }
                    pos++;       /* skip ':' */
                    pos += slen; /* skip string data */
                } else {
                    pos++;
                }
            }

            size_t info_end = pos;

            /* Hash the raw bytes of the info dict */
            sha1(raw + info_start, info_end - info_start, info_hash);

            bencode_free(info_node);
            return 0;
        }
    }

    fprintf(stderr, "torrent: 'info' key not found in torrent file\n");
    return -1;
}

/* torrent_parse */

TorrentInfo *torrent_parse(const char *path) {
    /* Step 1: Read the file */
    size_t   raw_len;
    uint8_t *raw = read_file(path, &raw_len);
    if (!raw) return NULL;

    /* Step 2: Parse the bencoding */
    BencodeNode *root = bencode_parse(raw, raw_len);
    if (!root) {
        fprintf(stderr, "torrent: failed to parse bencoding in %s\n", path);
        free(raw);
        return NULL;
    }

    if (root->type != BENCODE_DICT) {
        fprintf(stderr, "torrent: top-level value is not a dict\n");
        bencode_free(root);
        free(raw);
        return NULL;
    }

    /* Step 3: Allocate the TorrentInfo struct */
    TorrentInfo *t = xcalloc(1, sizeof(TorrentInfo));

    /* Step 4: Extract announce URL */
    BencodeNode *announce = bencode_dict_get(root, "announce");
    if (announce && announce->type == BENCODE_STR) {
        size_t len = announce->str.len < sizeof(t->announce) - 1
                   ? announce->str.len : sizeof(t->announce) - 1;
        memcpy(t->announce, announce->str.data, len);
        t->announce[len] = '\0';
    }

    /* Step 5: Extract announce-list (backup trackers) */
    BencodeNode *alist = bencode_dict_get(root, "announce-list");
    if (alist && alist->type == BENCODE_LIST) {
        for (size_t i = 0; i < alist->list.count && t->num_trackers < MAX_TRACKERS; i++) {
            BencodeNode *tier = alist->list.items[i];
            if (tier->type != BENCODE_LIST) continue;
            for (size_t j = 0; j < tier->list.count && t->num_trackers < MAX_TRACKERS; j++) {
                BencodeNode *url = tier->list.items[j];
                if (url->type != BENCODE_STR) continue;
                size_t len = url->str.len < 511 ? url->str.len : 511;
                memcpy(t->announce_list[t->num_trackers], url->str.data, len);
                t->announce_list[t->num_trackers][len] = '\0';
                t->num_trackers++;
            }
        }
    }

    /* Step 6: Extract optional comment and created-by */
    BencodeNode *comment = bencode_dict_get(root, "comment");
    if (comment && comment->type == BENCODE_STR) {
        size_t len = comment->str.len < sizeof(t->comment) - 1
                   ? comment->str.len : sizeof(t->comment) - 1;
        memcpy(t->comment, comment->str.data, len);
    }

    BencodeNode *created_by = bencode_dict_get(root, "created by");
    if (created_by && created_by->type == BENCODE_STR) {
        size_t len = created_by->str.len < sizeof(t->created_by) - 1
                   ? created_by->str.len : sizeof(t->created_by) - 1;
        memcpy(t->created_by, created_by->str.data, len);
    }

    /* Step 7: Parse the info dictionary */
    BencodeNode *info = bencode_dict_get(root, "info");
    if (!info || info->type != BENCODE_DICT) {
        fprintf(stderr, "torrent: missing or invalid 'info' dict\n");
        bencode_free(root);
        free(raw);
        free(t);
        return NULL;
    }

    /* name */
    BencodeNode *name = bencode_dict_get(info, "name");
    if (name && name->type == BENCODE_STR) {
        size_t len = name->str.len < sizeof(t->name) - 1
                   ? name->str.len : sizeof(t->name) - 1;
        memcpy(t->name, name->str.data, len);
        t->name[len] = '\0';
    }

    /* piece length */
    BencodeNode *pl = bencode_dict_get(info, "piece length");
    if (!pl || pl->type != BENCODE_INT) {
        fprintf(stderr, "torrent: missing 'piece length'\n");
        bencode_free(root); free(raw); free(t);
        return NULL;
    }
    t->piece_length = (int)pl->integer;

    /* pieces (concatenated SHA-1 hashes) */
    BencodeNode *pieces = bencode_dict_get(info, "pieces");
    if (!pieces || pieces->type != BENCODE_STR || pieces->str.len % 20 != 0) {
        fprintf(stderr, "torrent: invalid 'pieces' field\n");
        bencode_free(root); free(raw); free(t);
        return NULL;
    }
    t->num_pieces   = (int)(pieces->str.len / 20);
    t->pieces_hash  = xmalloc(pieces->str.len);
    memcpy(t->pieces_hash, pieces->str.data, pieces->str.len);

    /* Step 8: Single-file vs multi-file */
    BencodeNode *length_node = bencode_dict_get(info, "length");
    BencodeNode *files_node  = bencode_dict_get(info, "files");

    if (length_node && length_node->type == BENCODE_INT) {
        /* Single-file torrent */
        t->is_multi_file  = 0;
        t->total_length   = (long)length_node->integer;
        t->num_files      = 1;
        strncpy(t->files[0].path, t->name, MAX_PATH_LEN - 1);
        t->files[0].length = t->total_length;
        t->files[0].offset = 0;

    } else if (files_node && files_node->type == BENCODE_LIST) {
        /* Multi-file torrent */
        t->is_multi_file = 1;
        long offset = 0;

        for (size_t i = 0;
             i < files_node->list.count && t->num_files < MAX_FILES;
             i++) {

            BencodeNode *file = files_node->list.items[i];
            if (file->type != BENCODE_DICT) continue;

            BencodeNode *flen  = bencode_dict_get(file, "length");
            BencodeNode *fpath = bencode_dict_get(file, "path");

            if (!flen || flen->type != BENCODE_INT) continue;
            if (!fpath || fpath->type != BENCODE_LIST) continue;

            FileEntry *fe = &t->files[t->num_files];
            fe->length = (long)flen->integer;
            fe->offset = offset;
            offset    += fe->length;

            /* Build path from list of path components */
            size_t path_pos = 0;
            /* Prepend directory name */
            path_pos += snprintf(fe->path + path_pos,
                                 MAX_PATH_LEN - path_pos,
                                 "%s/", t->name);

            for (size_t j = 0; j < fpath->list.count; j++) {
                BencodeNode *comp = fpath->list.items[j];
                if (comp->type != BENCODE_STR) continue;
                if (j > 0) {
                    path_pos += snprintf(fe->path + path_pos,
                                         MAX_PATH_LEN - path_pos, "/");
                }
                size_t clen = comp->str.len < (MAX_PATH_LEN - path_pos - 1)
                            ? comp->str.len : (MAX_PATH_LEN - path_pos - 1);
                memcpy(fe->path + path_pos, comp->str.data, clen);
                path_pos += clen;
            }
            fe->path[path_pos] = '\0';

            t->num_files++;
            t->total_length += fe->length;
        }
    } else {
        fprintf(stderr, "torrent: no 'length' or 'files' in info dict\n");
        bencode_free(root); free(raw); free(t->pieces_hash); free(t);
        return NULL;
    }

    /* Step 9: Compute the info_hash from raw bytes */
    if (compute_info_hash(raw, raw_len, t->info_hash) < 0) {
        bencode_free(root); free(raw); free(t->pieces_hash); free(t);
        return NULL;
    }

    bencode_free(root);
    free(raw);
    return t;
}

/* torrent_free */

void torrent_free(TorrentInfo *t) {
    if (!t) return;
    free(t->pieces_hash);
    free(t);
}

/* torrent_print */

void torrent_print(const TorrentInfo *t) {
    char hash_str[41];
    hex_to_str(t->info_hash, 20, hash_str);

    printf("=== Torrent Info ===\n");
    printf("Name:         %s\n", t->name);
    printf("Comment:      %s\n", t->comment);
    printf("Created by:   %s\n", t->created_by);
    printf("Info hash:    %s\n", hash_str);
    printf("Announce:     %s\n", t->announce);
    printf("Total size:   %ld bytes (%.2f MB)\n",
           t->total_length, (double)t->total_length / (1024.0 * 1024.0));
    printf("Piece length: %d bytes (%.0f KiB)\n",
           t->piece_length, (double)t->piece_length / 1024.0);
    printf("Pieces:       %d\n", t->num_pieces);
    printf("Multi-file:   %s\n", t->is_multi_file ? "yes" : "no");

    if (t->num_trackers > 0) {
        printf("Backup trackers (%d):\n", t->num_trackers);
        for (int i = 0; i < t->num_trackers && i < 5; i++) {
            printf("  [%d] %s\n", i, t->announce_list[i]);
        }
    }

    if (t->is_multi_file) {
        printf("Files (%d):\n", t->num_files);
        for (int i = 0; i < t->num_files && i < 10; i++) {
            printf("  [%d] %s (%ld bytes)\n",
                   i, t->files[i].path, t->files[i].length);
        }
        if (t->num_files > 10) printf("  ... and %d more\n", t->num_files - 10);
    }
    printf("===================\n");
}

/* torrent_get_piece_length */

int torrent_get_piece_length(const TorrentInfo *t, int piece_idx) {
    /*
     * All pieces are t->piece_length bytes, EXCEPT the last piece which
     * may be smaller (if total_length is not a multiple of piece_length).
     *
     * Formula for last piece: total_length % piece_length
     * If that's 0, the last piece is exactly piece_length.
     */
    if (piece_idx < t->num_pieces - 1) {
        return t->piece_length;
    }
    /* Last piece */
    int remainder = (int)(t->total_length % t->piece_length);
    return remainder == 0 ? t->piece_length : remainder;
}

/* torrent_get_piece_hash */

const uint8_t *torrent_get_piece_hash(const TorrentInfo *t, int piece_idx) {
    return t->pieces_hash + (piece_idx * 20);
}
