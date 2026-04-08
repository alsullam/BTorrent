/**
 * pieces.c — Piece Download Manager
 *
 * Key improvements over v1:
 *   - Resume support: on startup, SHA-1 verify existing data on disk.
 *     Already-good pieces are marked COMPLETE without re-downloading.
 *   - Multi-file write: pieces that span file boundaries are split
 *     correctly across the individual output files.
 *   - Download speed tracking: reports bytes/sec in progress output.
 *   - MSG_HAVE tracking: provides a bitfield of completed pieces
 *     so we can announce to peers (seeding support groundwork).
 */

#define _POSIX_C_SOURCE 200809L
#include "pieces.h"
#include "peer.h"
#include "sha1.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/* Internal helpers */

static int compute_num_blocks(int piece_length) {
    return (piece_length + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

/* Multi-file output management */

/*
 * ensure_dir - create all directories in a path.
 * E.g. "ubuntu/server/disk1.iso" → creates "ubuntu/" and "ubuntu/server/".
 */
static void ensure_dir(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);  /* ignore errors — dir may exist */
            *p = '/';
        }
    }
}

/*
 * open_output_files - for single-file torrents, open one file.
 *   For multi-file torrents, open all files and pre-allocate them.
 *
 * Returns an array of FILE* of length pm->torrent->num_files,
 * or NULL on failure. Also sets pm->out_file (first file) for
 * backwards compatibility with single-file usage.
 */
static FILE **open_output_files(PieceManager *pm) {
    const TorrentInfo *t = pm->torrent;
    FILE **fds = xcalloc(t->num_files, sizeof(FILE *));

    for (int i = 0; i < t->num_files; i++) {
        const char *fpath = t->files[i].path;
        ensure_dir(fpath);

        /*
         * Try to open existing file for read+write (resume case).
         * If it doesn't exist, create it.
         */
        fds[i] = fopen(fpath, "r+b");
        if (!fds[i]) {
            fds[i] = fopen(fpath, "w+b");
            if (!fds[i]) {
                perror(fpath);
                /* Close already-opened files */
                for (int j = 0; j < i; j++) fclose(fds[j]);
                free(fds);
                return NULL;
            }
            /* Pre-allocate: seek to last byte and write a zero */
            if (t->files[i].length > 0) {
                if (fseek(fds[i], t->files[i].length - 1, SEEK_SET) != 0 ||
                    fputc(0, fds[i]) == EOF) {
                    fprintf(stderr, "pieces: pre-alloc failed for %s\n", fpath);
                }
                fflush(fds[i]);
            }
        }
    }
    return fds;
}

/* piece_manager_new */

PieceManager *piece_manager_new(const TorrentInfo *torrent,
                                const char        *out_path) {
    PieceManager *pm = xcalloc(1, sizeof(PieceManager));
    pm->torrent    = torrent;
    pm->num_pieces = torrent->num_pieces;
    pm->completed  = 0;
    strncpy(pm->out_path, out_path, sizeof(pm->out_path) - 1);

    /* Allocate piece status array */
    pm->pieces = xcalloc(pm->num_pieces, sizeof(PieceStatus));
    for (int i = 0; i < pm->num_pieces; i++) {
        pm->pieces[i].state        = PIECE_EMPTY;
        pm->pieces[i].data         = NULL;
        pm->pieces[i].piece_length = torrent_get_piece_length(torrent, i);
        pm->pieces[i].num_blocks   = compute_num_blocks(pm->pieces[i].piece_length);
        pm->pieces[i].blocks_done  = 0;
        pm->pieces[i].block_received =
            xcalloc(pm->pieces[i].num_blocks, sizeof(uint8_t));
    }

    /* Bitfield for announcing completed pieces to peers */
    int bf_bytes = (pm->num_pieces + 7) / 8;
    pm->our_bitfield = xcalloc(bf_bytes, 1);
    pm->bf_len       = bf_bytes;

    /* Open output files */
    pm->file_handles = open_output_files(pm);
    if (!pm->file_handles) {
        for (int i = 0; i < pm->num_pieces; i++)
            free(pm->pieces[i].block_received);
        free(pm->pieces);
        free(pm->our_bitfield);
        free(pm);
        return NULL;
    }

    /* Set pm->out_file to the first file handle for single-file compat */
    pm->out_file = pm->file_handles[0];

    printf("pieces: output %s (%d file%s, %.2f MB)\n",
           torrent->is_multi_file ? "directory" : "file",
           torrent->num_files,
           torrent->num_files == 1 ? "" : "s",
           (double)torrent->total_length / (1024.0 * 1024.0));

    /* Resume: verify pieces already on disk */
    int resumed = 0;
    uint8_t *buf = xmalloc(torrent->piece_length);

    for (int i = 0; i < pm->num_pieces; i++) {
        int plen = pm->pieces[i].piece_length;

        /* Read this piece's data from disk */
        int ok = piece_manager_read_piece(pm, i, buf);
        if (!ok) continue;

        /* SHA-1 verify */
        uint8_t hash[20];
        sha1(buf, (size_t)plen, hash);
        if (memcmp(hash, torrent_get_piece_hash(torrent, i), 20) == 0) {
            pm->pieces[i].state = PIECE_COMPLETE;
            pm->completed++;
            resumed++;

            /* Set bit in our bitfield */
            pm->our_bitfield[i / 8] |= (0x80 >> (i % 8));
        }
    }
    free(buf);

    if (resumed > 0) {
        printf("pieces: resumed %d/%d pieces from existing files\n",
               resumed, pm->num_pieces);
    }

    clock_gettime(CLOCK_MONOTONIC, &pm->start_time);
    pm->bytes_at_start = (long long)resumed * torrent->piece_length;

    return pm;
}

/* piece_manager_free */

void piece_manager_free(PieceManager *pm) {
    if (!pm) return;
    if (pm->file_handles) {
        for (int i = 0; i < pm->torrent->num_files; i++) {
            if (pm->file_handles[i]) fclose(pm->file_handles[i]);
        }
        free(pm->file_handles);
    }
    for (int i = 0; i < pm->num_pieces; i++) {
        free(pm->pieces[i].data);
        free(pm->pieces[i].block_received);
    }
    free(pm->pieces);
    free(pm->our_bitfield);
    free(pm);
}

/* Multi-file piece I/O */

/*
 * A piece occupies bytes [piece_idx * piece_length, piece_idx * piece_length + piece_len).
 * For multi-file torrents, this byte range may span multiple files.
 * Each file occupies bytes [file.offset, file.offset + file.length).
 *
 * We iterate over all files that overlap with the piece's byte range
 * and read/write the appropriate portions.
 */
static int rw_piece_multifile(PieceManager *pm, int piece_idx,
                              uint8_t *buf, int write_mode) {
    const TorrentInfo *t = pm->torrent;
    long piece_start = (long)piece_idx * t->piece_length;
    int  piece_len   = pm->pieces[piece_idx].piece_length;
    long piece_end   = piece_start + piece_len;

    int  buf_pos = 0;

    for (int fi = 0; fi < t->num_files; fi++) {
        long file_start = t->files[fi].offset;
        long file_end   = file_start + t->files[fi].length;

        /* Find overlap between piece range and this file's range */
        long ov_start = piece_start > file_start ? piece_start : file_start;
        long ov_end   = piece_end   < file_end   ? piece_end   : file_end;

        if (ov_start >= ov_end) continue;  /* no overlap with this file */

        long  file_offset = ov_start - file_start;
        int   chunk_len   = (int)(ov_end - ov_start);
        FILE *fd          = pm->file_handles[fi];

        if (!fd) continue;

        if (fseek(fd, file_offset, SEEK_SET) != 0) {
            perror("pieces: fseek");
            return 0;
        }

        if (write_mode) {
            if (fwrite(buf + buf_pos, 1, chunk_len, fd) != (size_t)chunk_len) {
                perror("pieces: fwrite");
                return 0;
            }
            fflush(fd);
        } else {
            size_t got = fread(buf + buf_pos, 1, chunk_len, fd);
            if ((int)got != chunk_len) return 0;  /* file shorter than expected */
        }
        buf_pos += chunk_len;
    }
    return (buf_pos == piece_len);
}

/* piece_manager_read_piece */

int piece_manager_read_piece(PieceManager *pm, int piece_idx, uint8_t *buf) {
    return rw_piece_multifile(pm, piece_idx, buf, 0 /* read */);
}

/* Internal: write verified piece to disk */

static void write_piece(PieceManager *pm, int piece_idx) {
    PieceStatus *ps = &pm->pieces[piece_idx];

    if (!rw_piece_multifile(pm, piece_idx, ps->data, 1 /* write */)) {
        fprintf(stderr, "pieces: write failed for piece %d\n", piece_idx);
        return;
    }

    free(ps->data);
    ps->data  = NULL;
    ps->state = PIECE_COMPLETE;
    pm->completed++;

    /* Update our bitfield so we can announce this piece to peers */
    pm->our_bitfield[piece_idx / 8] |= (0x80 >> (piece_idx % 8));

    piece_manager_print_progress(pm);
}

/* piece_manager_on_block */

int piece_manager_on_block(PieceManager  *pm,
                           int            piece_idx,
                           int            begin,
                           const uint8_t *data,
                           int            len) {
    if (piece_idx < 0 || piece_idx >= pm->num_pieces) return 0;

    PieceStatus *ps = &pm->pieces[piece_idx];
    if (ps->state == PIECE_COMPLETE) return 1;

    /* Activate piece buffer on first block */
    if (ps->state == PIECE_EMPTY) {
        ps->data  = xmalloc((size_t)ps->piece_length);
        ps->state = PIECE_ACTIVE;
    }

    if (begin < 0 || begin + len > ps->piece_length) return 0;

    memcpy(ps->data + begin, data, (size_t)len);

    int block_idx = begin / BLOCK_SIZE;
    if (block_idx < ps->num_blocks && !ps->block_received[block_idx]) {
        ps->block_received[block_idx] = 1;
        ps->blocks_done++;
        pm->bytes_downloaded += len;
    }

    if (ps->blocks_done < ps->num_blocks) return 0;

    /* All blocks arrived — verify SHA-1 */
    uint8_t computed[20];
    sha1(ps->data, (size_t)ps->piece_length, computed);
    if (memcmp(computed, torrent_get_piece_hash(pm->torrent, piece_idx), 20) != 0) {
        fprintf(stderr, "pieces: piece %d SHA-1 FAILED — retrying\n", piece_idx);
        free(ps->data); ps->data = NULL;
        ps->state = PIECE_EMPTY;
        memset(ps->block_received, 0, ps->num_blocks);
        ps->blocks_done = 0;
        return -1;
    }

    write_piece(pm, piece_idx);
    return 1;
}

/* piece_manager_next_needed */

int piece_manager_next_needed(PieceManager  *pm,
                              const uint8_t *peer_bitfield,
                              int            num_pieces) {
    for (int i = 0; i < pm->num_pieces; i++) {
        if (pm->pieces[i].state != PIECE_EMPTY)  continue;
        if (peer_bitfield) {
            if (i < num_pieces && !bitfield_has_piece(peer_bitfield, i))
                continue;
        }
        return i;
    }
    return -1;
}

/* piece_manager_is_complete */

int piece_manager_is_complete(const PieceManager *pm) {
    return pm->completed == pm->num_pieces;
}

/* piece_manager_print_progress */

void piece_manager_print_progress(const PieceManager *pm) {
    int total   = pm->num_pieces;
    int done    = pm->completed;
    int pct     = total > 0 ? (done * 100) / total : 0;
    int bar_w   = 36;
    int filled  = (done * bar_w) / (total > 0 ? total : 1);

    /* Download speed: bytes since start / elapsed seconds */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec  - pm->start_time.tv_sec) +
        (now.tv_nsec - pm->start_time.tv_nsec) / 1e9;
    double speed_kbs = elapsed > 0.5
        ? (double)pm->bytes_downloaded / elapsed / 1024.0
        : 0.0;

    /* ETA */
    long remaining_bytes =
        (long)(pm->torrent->total_length) -
        (long)((long long)done * pm->torrent->piece_length);
    if (remaining_bytes < 0) remaining_bytes = 0;
    double eta_s = (speed_kbs > 0.1)
        ? (double)remaining_bytes / (speed_kbs * 1024.0)
        : -1;

    printf("\r[");
    for (int i = 0; i < bar_w; i++) printf(i < filled ? "#" : ".");
    printf("] %3d%% (%d/%d) %.1f KB/s", pct, done, total, speed_kbs);
    if (eta_s >= 0) {
        int eta_m = (int)(eta_s / 60);
        int eta_s2 = (int)eta_s % 60;
        printf(" ETA %dm%02ds", eta_m, eta_s2);
    }
    printf("  ");
    fflush(stdout);

    if (done == total) printf("\n");
}
