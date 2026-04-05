/**
 * pieces.h — Piece Download Manager
 *
 * Manages the state of all pieces during a download:
 *   - Which pieces do we still need?
 *   - Which blocks within a piece have arrived?
 *   - When a piece is complete, verify its SHA-1 and write to disk
 *
 * See docs/06_pieces.md for full explanation.
 *
 * Usage:
 *   PieceManager *pm = piece_manager_new(torrent, "output.iso");
 *
 *   // When a block arrives from a peer:
 *   piece_manager_on_block(pm, piece_idx, begin, data, data_len);
 *
 *   // Select the next piece to download from a peer:
 *   int next = piece_manager_next_needed(pm, peer_bitfield);
 *
 *   // Check if everything is done:
 *   if (piece_manager_is_complete(pm)) { ... }
 *
 *   piece_manager_free(pm);
 */

#ifndef BTORRENT_PIECES_H
#define BTORRENT_PIECES_H

#include "torrent.h"
#include <stdint.h>
#include <stdio.h>

/* ── Block constants ─────────────────────────────────────────────────────── */

/**
 * BLOCK_SIZE - the standard block request size (16 KiB).
 *
 * Pieces are large (often 256 KiB – 1 MiB), but we request them in
 * 16 KiB sub-pieces called "blocks". This is the standard de facto size
 * used by all BitTorrent clients.
 *
 * Why 16 KiB? It's small enough to pipeline efficiently, large enough
 * not to create too much overhead.
 */
#define BLOCK_SIZE 16384

/* ── Piece states ────────────────────────────────────────────────────────── */

typedef enum {
    PIECE_EMPTY,      /* not started */
    PIECE_ACTIVE,     /* currently being downloaded */
    PIECE_COMPLETE,   /* downloaded, verified, written to disk */
} PieceState;

/* ── Per-piece tracking ──────────────────────────────────────────────────── */

/**
 * PieceStatus - tracks download state of one piece.
 */
typedef struct {
    PieceState  state;

    uint8_t    *data;           /* piece data buffer (NULL until active) */
    int         piece_length;   /* actual byte length of this piece */

    uint8_t    *block_received; /* bool[num_blocks]: which blocks arrived */
    int         num_blocks;     /* ceil(piece_length / BLOCK_SIZE) */
    int         blocks_done;    /* how many blocks received so far */
} PieceStatus;

/* ── The piece manager ───────────────────────────────────────────────────── */

typedef struct {
    const TorrentInfo *torrent;  /* torrent metadata (not owned) */
    PieceStatus       *pieces;   /* array of num_pieces PieceStatus */
    int                num_pieces;

    /* Output file */
    FILE  *out_file;             /* open file handle for output */
    char   out_path[1024];       /* path to output file */

    /* Progress tracking */
    int    completed;            /* number of pieces fully written */
} PieceManager;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * piece_manager_new - create a piece manager and open the output file.
 *
 * Pre-allocates the output file to the correct size (so we can seek and
 * write pieces out of order without issue).
 *
 * @torrent    parsed torrent info
 * @out_path   path to write the downloaded file
 *
 * @return heap-allocated PieceManager, or NULL on error
 */
PieceManager *piece_manager_new(const TorrentInfo *torrent,
                                const char        *out_path);

/**
 * piece_manager_free - free the PieceManager and close the output file.
 */
void piece_manager_free(PieceManager *pm);

/* ── Block reception ─────────────────────────────────────────────────────── */

/**
 * piece_manager_on_block - handle a received block of data.
 *
 * Called when a MSG_PIECE message arrives from a peer.
 *
 * @pm           the piece manager
 * @piece_index  which piece this block belongs to
 * @begin        byte offset within the piece
 * @data         the block data
 * @len          length of data
 *
 * Internally:
 *   1. Copy data into piece buffer
 *   2. Mark block as received
 *   3. If all blocks received → SHA-1 verify → write to disk (or retry)
 *
 * @return 1 if piece completed and verified, 0 if more blocks needed,
 *         -1 if piece failed verification (will be re-attempted)
 */
int piece_manager_on_block(PieceManager *pm,
                           int           piece_index,
                           int           begin,
                           const uint8_t *data,
                           int           len);

/* ── Piece selection ─────────────────────────────────────────────────────── */

/**
 * piece_manager_next_needed - find the next piece index we need.
 *
 * If `peer_bitfield` is non-NULL, only consider pieces the peer has.
 * Otherwise, return any needed piece.
 *
 * Strategy: sequential (simplest). A real client uses "rarest first".
 *
 * @pm              the piece manager
 * @peer_bitfield   the peer's bitfield (from MSG_BITFIELD), or NULL
 * @num_pieces      number of pieces (needed to interpret bitfield)
 *
 * @return piece index to download, or -1 if none available
 */
int piece_manager_next_needed(PieceManager  *pm,
                              const uint8_t *peer_bitfield,
                              int            num_pieces);

/* ── Progress ────────────────────────────────────────────────────────────── */

/**
 * piece_manager_is_complete - returns 1 if all pieces are downloaded.
 */
int piece_manager_is_complete(const PieceManager *pm);

/**
 * piece_manager_print_progress - print a progress bar to stdout.
 *
 * Example: [##########..........] 50% (25/50 pieces)
 */
void piece_manager_print_progress(const PieceManager *pm);

#endif /* BTORRENT_PIECES_H */
