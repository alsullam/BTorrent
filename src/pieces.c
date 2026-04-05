/**
 * pieces.c — Piece Download Manager
 *
 * Manages all downloaded pieces: which are needed, which blocks
 * have arrived, SHA-1 verification, and writing to disk.
 *
 * See docs/06_pieces.md for a full explanation.
 */

#include "../include/pieces.h"
#include "../include/peer.h"
#include "sha1.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

/*
 * compute_num_blocks - how many BLOCK_SIZE blocks fit in a piece?
 *
 * The last block of a piece may be smaller than BLOCK_SIZE.
 * ceil(piece_length / BLOCK_SIZE) = (piece_length + BLOCK_SIZE - 1) / BLOCK_SIZE
 */
static int compute_num_blocks(int piece_length) {
    return (piece_length + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

/* ── piece_manager_new ───────────────────────────────────────────────────── */

PieceManager *piece_manager_new(const TorrentInfo *torrent,
                                const char        *out_path) {
    PieceManager *pm = xcalloc(1, sizeof(PieceManager));
    pm->torrent    = torrent;
    pm->num_pieces = torrent->num_pieces;
    pm->completed  = 0;
    strncpy(pm->out_path, out_path, sizeof(pm->out_path) - 1);

    /* Allocate piece status array — one entry per piece */
    pm->pieces = xcalloc(pm->num_pieces, sizeof(PieceStatus));

    for (int i = 0; i < pm->num_pieces; i++) {
        pm->pieces[i].state        = PIECE_EMPTY;
        pm->pieces[i].data         = NULL;  /* allocated on demand */
        pm->pieces[i].piece_length = torrent_get_piece_length(torrent, i);
        pm->pieces[i].num_blocks   = compute_num_blocks(pm->pieces[i].piece_length);
        pm->pieces[i].blocks_done  = 0;
        pm->pieces[i].block_received =
            xcalloc(pm->pieces[i].num_blocks, sizeof(uint8_t));
    }

    /*
     * Open/create the output file.
     *
     * We use "r+b" (read-write binary) if file exists, "w+b" if not.
     * Then we pre-allocate it to the full size so we can seek and write
     * pieces in any order without gaps.
     *
     * Pre-allocation via fseek + fwrite 0 at the end creates a
     * sparse file on most modern filesystems (no actual disk space
     * used until we write the real data).
     */
    pm->out_file = fopen(out_path, "w+b");
    if (!pm->out_file) {
        perror(out_path);
        free(pm->pieces);
        free(pm);
        return NULL;
    }

    /* Pre-allocate file: seek to last byte and write a zero */
    if (fseek(pm->out_file, torrent->total_length - 1, SEEK_SET) != 0 ||
        fputc(0, pm->out_file) == EOF) {
        fprintf(stderr, "pieces: failed to pre-allocate %ld bytes for %s\n",
                torrent->total_length, out_path);
        fclose(pm->out_file);
        free(pm->pieces);
        free(pm);
        return NULL;
    }
    fflush(pm->out_file);

    printf("pieces: output file created: %s (%.2f MB)\n",
           out_path, (double)torrent->total_length / (1024.0 * 1024.0));

    return pm;
}

/* ── piece_manager_free ──────────────────────────────────────────────────── */

void piece_manager_free(PieceManager *pm) {
    if (!pm) return;

    if (pm->out_file) fclose(pm->out_file);

    for (int i = 0; i < pm->num_pieces; i++) {
        free(pm->pieces[i].data);
        free(pm->pieces[i].block_received);
    }
    free(pm->pieces);
    free(pm);
}

/* ── Internal: activate a piece (allocate its data buffer) ──────────────── */

static void activate_piece(PieceManager *pm, int piece_index) {
    PieceStatus *ps = &pm->pieces[piece_index];
    if (ps->state != PIECE_EMPTY) return;

    ps->data  = xmalloc((size_t)ps->piece_length);
    ps->state = PIECE_ACTIVE;
    memset(ps->block_received, 0, ps->num_blocks);
    ps->blocks_done = 0;
}

/* ── Internal: verify piece SHA-1 ────────────────────────────────────────── */

static int verify_piece(const TorrentInfo *t, int piece_index,
                        const uint8_t *data, int data_len) {
    /*
     * Compute SHA-1 of the piece data and compare with the expected hash
     * from the torrent file.
     *
     * The expected hash is stored as 20 raw bytes at:
     *   t->pieces_hash + (piece_index * 20)
     */
    uint8_t computed[20];
    sha1(data, (size_t)data_len, computed);

    const uint8_t *expected = torrent_get_piece_hash(t, piece_index);
    return memcmp(computed, expected, 20) == 0;
}

/* ── Internal: write a verified piece to disk ────────────────────────────── */

static void write_piece(PieceManager *pm, int piece_index) {
    PieceStatus *ps     = &pm->pieces[piece_index];
    long         offset = (long)piece_index * pm->torrent->piece_length;

    /*
     * Seek to the piece's byte position in the output file and write.
     *
     * For multi-file torrents this works because we treat the entire
     * download as one contiguous byte range (files are concatenated).
     */
    if (fseek(pm->out_file, offset, SEEK_SET) != 0) {
        perror("fseek");
        return;
    }

    size_t written = fwrite(ps->data, 1, (size_t)ps->piece_length, pm->out_file);
    if (written != (size_t)ps->piece_length) {
        perror("fwrite");
        return;
    }
    fflush(pm->out_file);

    /* Free the piece buffer now — we've written it to disk */
    free(ps->data);
    ps->data  = NULL;
    ps->state = PIECE_COMPLETE;
    pm->completed++;

    piece_manager_print_progress(pm);
}

/* ── piece_manager_on_block ──────────────────────────────────────────────── */

int piece_manager_on_block(PieceManager  *pm,
                           int            piece_index,
                           int            begin,
                           const uint8_t *data,
                           int            len) {

    if (piece_index < 0 || piece_index >= pm->num_pieces) {
        fprintf(stderr, "pieces: invalid piece index %d\n", piece_index);
        return 0;
    }

    PieceStatus *ps = &pm->pieces[piece_index];

    /* If piece was empty, activate it now */
    if (ps->state == PIECE_EMPTY) {
        activate_piece(pm, piece_index);
    }

    /* Ignore blocks for completed pieces (can happen due to network re-sends) */
    if (ps->state == PIECE_COMPLETE) return 1;

    /* Validate block bounds */
    if (begin < 0 || begin + len > ps->piece_length) {
        fprintf(stderr, "pieces: block out of bounds: piece=%d begin=%d len=%d\n",
                piece_index, begin, len);
        return 0;
    }

    /* Copy block data into piece buffer */
    memcpy(ps->data + begin, data, (size_t)len);

    /* Mark this block as received */
    int block_idx = begin / BLOCK_SIZE;
    if (block_idx < ps->num_blocks && !ps->block_received[block_idx]) {
        ps->block_received[block_idx] = 1;
        ps->blocks_done++;
    }

    /* Check if all blocks for this piece have arrived */
    if (ps->blocks_done < ps->num_blocks) {
        return 0; /* not done yet */
    }

    /*
     * All blocks received! Verify SHA-1 integrity.
     *
     * If verification fails, the piece was corrupted in transit.
     * We reset and will re-request it from another peer.
     */
    if (!verify_piece(pm->torrent, piece_index, ps->data, ps->piece_length)) {
        fprintf(stderr, "pieces: piece %d FAILED SHA-1 — re-downloading\n",
                piece_index);

        /* Reset piece state */
        free(ps->data);
        ps->data  = NULL;
        ps->state = PIECE_EMPTY;
        memset(ps->block_received, 0, ps->num_blocks);
        ps->blocks_done = 0;
        return -1;
    }

    /* Verification passed — write to disk */
    write_piece(pm, piece_index);
    return 1;
}

/* ── piece_manager_next_needed ───────────────────────────────────────────── */

int piece_manager_next_needed(PieceManager  *pm,
                              const uint8_t *peer_bitfield,
                              int            num_pieces) {
    /*
     * Sequential piece selection: find the first piece we still need
     * that the peer also has.
     *
     * A real BitTorrent client uses "rarest first" — preferring pieces
     * that fewer peers have, to improve swarm health. Sequential is
     * simpler to implement and great for learning.
     */
    for (int i = 0; i < pm->num_pieces; i++) {
        /* Skip pieces we already have */
        if (pm->pieces[i].state == PIECE_COMPLETE) continue;
        /* Skip pieces currently being downloaded */
        if (pm->pieces[i].state == PIECE_ACTIVE) continue;

        /* If we have a bitfield, check that the peer has this piece */
        if (peer_bitfield) {
            if (i < num_pieces && !bitfield_has_piece(peer_bitfield, i)) {
                continue; /* peer doesn't have this piece */
            }
        }

        return i; /* found a needed piece the peer has */
    }

    return -1; /* no more pieces needed from this peer */
}

/* ── piece_manager_is_complete ───────────────────────────────────────────── */

int piece_manager_is_complete(const PieceManager *pm) {
    return pm->completed == pm->num_pieces;
}

/* ── piece_manager_print_progress ────────────────────────────────────────── */

void piece_manager_print_progress(const PieceManager *pm) {
    int   total   = pm->num_pieces;
    int   done    = pm->completed;
    int   percent = total > 0 ? (done * 100) / total : 0;
    int   bar_w   = 40;
    int   filled  = (done * bar_w) / (total > 0 ? total : 1);

    printf("\r[");
    for (int i = 0; i < bar_w; i++) {
        printf(i < filled ? "#" : ".");
    }
    printf("] %3d%% (%d/%d pieces)", percent, done, total);
    fflush(stdout);

    if (done == total) printf("\n");
}
