/**
 * pieces.h — Piece Download Manager
 *
 * v2 additions:
 *   - Resume support via on-disk SHA-1 verification at startup
 *   - Multi-file write: pieces are split across output files at correct offsets
 *   - Download speed + ETA in progress display
 *   - Our bitfield: tracks which pieces we have (for announcing to peers)
 */

#ifndef BTORRENT_PIECES_H
#define BTORRENT_PIECES_H

#include "torrent.h"
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define BLOCK_SIZE 16384   /* 16 KiB — standard BitTorrent block request size */

/* Piece states */

typedef enum {
    PIECE_EMPTY,     /* not started, no buffer */
    PIECE_ACTIVE,    /* downloading, buffer allocated */
    PIECE_COMPLETE,  /* verified and written to disk */
} PieceState;

/* Per-piece tracking */

typedef struct {
    PieceState  state;
    uint8_t    *data;           /* piece data buffer (NULL unless ACTIVE) */
    int         piece_length;   /* actual byte length of this piece */
    uint8_t    *block_received; /* bool[num_blocks] */
    int         num_blocks;     /* ceil(piece_length / BLOCK_SIZE) */
    int         blocks_done;
} PieceStatus;

/* Piece manager */

typedef struct {
    const TorrentInfo *torrent;
    PieceStatus       *pieces;
    int                num_pieces;

    /* Output file handles — one per file in the torrent */
    FILE  **file_handles;       /* array of num_files file handles */
    FILE   *out_file;           /* alias for file_handles[0] (single-file compat) */
    char    out_path[1024];     /* output path (dir name for multi-file) */

    /* Progress */
    int               completed;
    long long         bytes_downloaded;  /* bytes received this session */
    long long         bytes_at_start;    /* bytes already on disk at resume */
    struct timespec   start_time;        /* for speed calculation */

    /* Our bitfield — set bits = pieces we have.
     * Used to send MSG_BITFIELD when other peers connect to us. */
    uint8_t *our_bitfield;
    int      bf_len;           /* length of our_bitfield in bytes */

} PieceManager;

/* Lifecycle */

/**
 * piece_manager_new - create manager, open output files, resume if possible.
 *
 * On startup, existing output files are SHA-1 verified piece by piece.
 * Any piece that already matches its hash is marked COMPLETE and skipped
 * during the download. This enables seamless resume after interruption.
 *
 * For multi-file torrents, out_path is the base directory name.
 */
PieceManager *piece_manager_new(const TorrentInfo *torrent,
                                const char        *out_path);

void piece_manager_free(PieceManager *pm);

/* Block reception */

/**
 * piece_manager_on_block - handle a received 16 KiB block.
 *
 * @return  1 = piece complete + verified
 *          0 = more blocks needed
 *         -1 = piece SHA-1 failed (reset and will retry)
 */
int piece_manager_on_block(PieceManager *pm, int piece_index,
                           int begin, const uint8_t *data, int len);

/* Piece selection */

int piece_manager_next_needed(PieceManager  *pm,
                              const uint8_t *peer_bitfield,
                              int            num_pieces);

/* Status */

int  piece_manager_is_complete(const PieceManager *pm);
void piece_manager_print_progress(const PieceManager *pm);

/* Disk I/O */

/**
 * piece_manager_read_piece - read one piece from disk into buf.
 * Used during resume verification and when seeding.
 * buf must be at least piece_length bytes.
 * @return 1 on success, 0 on failure.
 */
int piece_manager_read_piece(PieceManager *pm, int piece_idx, uint8_t *buf);

#endif /* BTORRENT_PIECES_H */
