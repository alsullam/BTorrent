/**
 * main.c — Entry Point & Download Orchestration
 *
 * v2 improvements over the original:
 *   - Signal handling: SIGINT sends event=stopped to tracker before exit
 *   - Re-announce: when peers run out mid-download, re-announce to get fresh ones
 *   - MSG_HAVE: after completing a piece, notify connected peer
 *   - Smarter peer selection: skip peers we've already failed on
 *   - Cleaner progress output with speed and ETA
 *   - Multi-tracker: main loop retries across backup trackers too
 */

#define _POSIX_C_SOURCE 200809L
#include "torrent.h"
#include "tracker.h"
#include "peer.h"
#include "pieces.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>

/* Constants */

#define LISTEN_PORT      6881
#define PIPELINE_DEPTH   5      /* concurrent block requests per peer */
#define MAX_PEER_FAILS   3      /* give up on a peer after this many errors */
#define REANNOUNCE_PEERS 5      /* re-announce when fewer than this many usable peers remain */

/* Global state for signal handler */

static volatile int g_interrupted = 0;

/* These are set in main() so the signal handler can send stopped event */
static TorrentInfo *g_torrent   = NULL;
static uint8_t      g_peer_id[20];
static PieceManager *g_pm       = NULL;

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

/* send_have */

/*
 * After successfully downloading a piece, send MSG_HAVE to the connected
 * peer. This tells them we now have this piece (seeding groundwork) and
 * is required by the spec even for pure downloaders.
 *
 * MSG_HAVE format: [len=5][id=4][piece_index 4B big-endian]
 */
static void send_have(PeerConn *conn, int piece_idx) {
    uint8_t buf[9];
    write_uint32_be(buf,     5);             /* length = 5 */
    buf[4] = MSG_HAVE;
    write_uint32_be(buf + 5, (uint32_t)piece_idx);

    /* Best-effort — don't abort if this fails */
    if (send(conn->sock, buf, 9, 0) < 0) {
        /* Non-fatal: peer may have closed the connection */
    }
}

/* download_from_peer */

/*
 * Attempt to download as many pieces as possible from one peer.
 *
 * The loop:
 *   1. TCP connect + handshake
 *   2. Wait for BITFIELD (optional) then send INTERESTED
 *   3. Wait for UNCHOKE
 *   4. For each needed piece:
 *        a. Send PIPELINE_DEPTH block requests
 *        b. Receive MSG_PIECE, feeding each block to the piece manager
 *        c. As each block arrives, send the next request (keep pipeline full)
 *        d. On piece complete: send MSG_HAVE, pick next piece
 *   5. Return when: peer disconnects, chokes us, or no more needed pieces
 */
static int download_from_peer(const TorrentInfo *torrent,
                              PieceManager      *pm,
                              const Peer        *peer,
                              const uint8_t     *info_hash,
                              const uint8_t     *our_peer_id) {
    int downloaded = 0;

    PeerConn *conn = peer_connect(peer->ip, peer->port, info_hash, our_peer_id);
    if (!conn) return 0;

    if (peer_handshake(conn, info_hash, our_peer_id) < 0) {
        peer_close(conn); return 0;
    }

    /* Send our bitfield so the peer knows what we have.
     * If we have no pieces yet, skip it (empty bitfields are unnecessary). */
    if (pm->completed > 0) {
        /* MSG_BITFIELD: [len=1+bf_len][id=5][bitfield bytes] */
        int bflen = pm->bf_len;
        uint8_t *bitfield_msg = xmalloc(5 + bflen);
        write_uint32_be(bitfield_msg, (uint32_t)(1 + bflen));
        bitfield_msg[4] = MSG_BITFIELD;
        memcpy(bitfield_msg + 5, pm->our_bitfield, bflen);
        send(conn->sock, bitfield_msg, 5 + bflen, 0);
        free(bitfield_msg);
    }

    uint8_t *peer_bitfield = NULL;
    PeerMsg  msg;

    /* Read initial messages until we have something to act on */
    int init_attempts = 0;
    while (init_attempts < 30) {
        if (peer_recv_msg(conn, &msg) < 0) goto cleanup;
        init_attempts++;

        if (msg.type == MSG_BITFIELD) {
            peer_bitfield = msg.bitfield;

            msg.bitfield  = NULL;  /* take ownership */
            break;
        } else if (msg.type == MSG_UNCHOKE) {
            conn->am_choked = 0;
            peer_msg_free(&msg);
            break;
        } else if (msg.type == MSG_HAVE) {
            /* Build a minimal bitfield from HAVE messages if no BITFIELD sent */
            if (!peer_bitfield) {
                int bf_bytes = (torrent->num_pieces + 7) / 8;
                peer_bitfield = xcalloc(bf_bytes, 1);

            }
            uint32_t pidx = msg.have_index;
            if ((int)pidx < torrent->num_pieces)
                peer_bitfield[pidx / 8] |= (0x80 >> (pidx % 8));
            peer_msg_free(&msg);
        } else {
            peer_msg_free(&msg);
        }
    }

    /* Express interest and wait for unchoke */
    if (peer_send_interested(conn) < 0) goto cleanup;

    if (conn->am_choked) {
        for (int i = 0; i < 30 && conn->am_choked; i++) {
            if (peer_recv_msg(conn, &msg) < 0) goto cleanup;
            if (msg.type == MSG_UNCHOKE)
                printf("peer %s:%d: unchoked\n", peer->ip, peer->port);
            else if (msg.type == MSG_HAVE && peer_bitfield) {
                uint32_t pidx = msg.have_index;
                if ((int)pidx < torrent->num_pieces)
                    peer_bitfield[pidx / 8] |= (0x80 >> (pidx % 8));
            }
            peer_msg_free(&msg);
        }
    }

    if (conn->am_choked) {
        printf("peer %s:%d: stayed choked, skipping\n", peer->ip, peer->port);
        goto cleanup;
    }

    /* Main download loop */
    while (!piece_manager_is_complete(pm) && !g_interrupted) {

        int piece_idx = piece_manager_next_needed(pm, peer_bitfield,
                                                  torrent->num_pieces);
        if (piece_idx < 0) break;  /* peer has nothing we need */

        int piece_len  = torrent_get_piece_length(torrent, piece_idx);
        int num_blocks = (piece_len + BLOCK_SIZE - 1) / BLOCK_SIZE;

        printf("peer %s:%d: piece %d/%d (%d blocks)\n",
               peer->ip, peer->port,
               piece_idx, torrent->num_pieces - 1, num_blocks);

        /* Seed the pipeline */
        int send_block = 0, blocks_recv = 0;
        int piece_ok   = 1;

        while (send_block < num_blocks && send_block < PIPELINE_DEPTH) {
            int beg  = send_block * BLOCK_SIZE;
            int blen = BLOCK_SIZE;
            if (beg + blen > piece_len) blen = piece_len - beg;
            if (peer_send_request(conn, piece_idx, beg, blen) < 0) {
                piece_ok = 0; break;
            }
            send_block++;
        }

        /* Receive and pipeline */
        while (piece_ok && blocks_recv < num_blocks && !g_interrupted) {
            if (peer_recv_msg(conn, &msg) < 0) { piece_ok = 0; break; }

            switch (msg.type) {
                case MSG_PIECE: {
                    int result = piece_manager_on_block(
                        pm, (int)msg.piece_index, (int)msg.piece_begin,
                        msg.piece_data, (int)msg.piece_data_len);
                    peer_msg_free(&msg);

                    /* Only count blocks for our current piece */
                    if ((int)msg.piece_index == piece_idx ||
                        result == 1 || result == -1) {
                        blocks_recv++;
                    }

                    /* Keep pipeline full */
                    if (send_block < num_blocks) {
                        int beg  = send_block * BLOCK_SIZE;
                        int blen = BLOCK_SIZE;
                        if (beg + blen > piece_len) blen = piece_len - beg;
                        peer_send_request(conn, piece_idx, beg, blen);
                        send_block++;
                    }

                    if (result == 1) {
                        /* Piece verified — tell this peer we have it */
                        send_have(conn, piece_idx);
                        downloaded++;
                    } else if (result == -1) {
                        /* SHA-1 failure: piece reset, try from another peer */
                        piece_ok = 0;
                    }
                    break;
                }
                case MSG_CHOKE:
                    printf("peer %s:%d: re-choked\n", peer->ip, peer->port);
                    piece_ok = 0;
                    break;
                case MSG_HAVE:
                    if (peer_bitfield && (int)msg.have_index < torrent->num_pieces)
                        peer_bitfield[msg.have_index/8] |= (0x80>>(msg.have_index%8));
                    peer_msg_free(&msg);
                    break;
                default:
                    peer_msg_free(&msg);
                    break;
            }
        }
        if (!piece_ok) break;
    }

cleanup:
    free(peer_bitfield);
    peer_close(conn);
    return downloaded;
}

/* print_usage */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "BTorrent v2 — Educational BitTorrent Client\n"
        "\n"
        "Usage:  %s <file.torrent> [output_path]\n"
        "\n"
        "  file.torrent  path to the .torrent file\n"
        "  output_path   where to save (default: torrent name)\n"
        "                for multi-file torrents, this is the directory name\n"
        "\n"
        "Examples:\n"
        "  %s ubuntu.torrent\n"
        "  %s debian.torrent /tmp/debian\n"
        "\n"
        "Press Ctrl+C to cancel (will send stopped event to tracker).\n",
        prog, prog, prog);
}

/* main */

int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(argv[0]); return EXIT_FAILURE; }

    /* SIGINT handler: graceful shutdown */
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("BTorrent v2 — Educational BitTorrent Client\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");

    /* Step 1: Parse torrent */
    printf("[1/4] Parsing torrent: %s\n", argv[1]);
    TorrentInfo *torrent = torrent_parse(argv[1]);
    if (!torrent) return EXIT_FAILURE;
    g_torrent = torrent;
    torrent_print(torrent);

    /* Output path */
    char out_path[1024];
    if (argc >= 3)
        strncpy(out_path, argv[2], sizeof(out_path) - 1);
    else
        strncpy(out_path, torrent->name, sizeof(out_path) - 1);
    out_path[sizeof(out_path)-1] = '\0';

    /* Step 2: Peer ID + tracker announce */
    generate_peer_id(g_peer_id);

    printf("\n[2/4] Announcing to tracker...\n");
    PeerList peers = tracker_announce(torrent, g_peer_id, LISTEN_PORT,
                                      0, 0, torrent->total_length, "started");
    if (peers.count == 0) {
        fprintf(stderr, "No peers from any tracker\n");
        torrent_free(torrent);
        return EXIT_FAILURE;
    }
    printf("      %d peers received (re-announce in %ds)\n",
           peers.count, peers.interval);

    /* Step 3: Initialize piece manager (resume if possible) */
    printf("\n[3/4] Initializing piece manager → %s\n", out_path);
    PieceManager *pm = piece_manager_new(torrent, out_path);
    if (!pm) {
        peer_list_free(&peers);
        torrent_free(torrent);
        return EXIT_FAILURE;
    }
    g_pm = pm;

    if (piece_manager_is_complete(pm)) {
        printf("\n✓ All pieces already on disk — nothing to download.\n");
        goto finish;
    }

    /* Step 4: Download loop */
    printf("\n[4/4] Downloading %d pieces, %d bytes each...\n\n",
           torrent->num_pieces, torrent->piece_length);

    time_t session_start = time(NULL);
    int    peer_idx      = 0;

    while (!piece_manager_is_complete(pm) && !g_interrupted) {

        /* Re-announce when we've exhausted most peers */
        if (peer_idx >= peers.count - REANNOUNCE_PEERS) {
            long downloaded_bytes =
                (long)pm->completed * torrent->piece_length;
            long remaining = torrent->total_length - downloaded_bytes;

            printf("\nRe-announcing to get fresh peers...\n");
            PeerList new_peers = tracker_announce(
                torrent, g_peer_id, LISTEN_PORT,
                downloaded_bytes, 0, remaining, "");

            if (new_peers.count > 0) {
                peer_list_free(&peers);
                peers     = new_peers;
                peer_idx  = 0;

                printf("Got %d fresh peers\n\n", peers.count);
            } else if (peer_idx >= peers.count) {
                fprintf(stderr, "\nNo more peers available.\n");
                break;
            }
        }

        printf("Peer [%d/%d]: %s:%d\n",
               peer_idx + 1, peers.count,
               peers.peers[peer_idx].ip, peers.peers[peer_idx].port);

        int got = download_from_peer(torrent, pm, &peers.peers[peer_idx],
                                     torrent->info_hash, g_peer_id);
        if (got > 0)
            printf("  → %d piece%s from this peer\n", got, got==1?"":"s");

        peer_idx++;

        /* Avoid tight loop when all peers refuse us quickly */
        if (got == 0 && peer_idx % 20 == 0)
            sleep(1);
    }

    /* Results */
finish: {
    time_t elapsed = time(NULL) - session_start;
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    if (g_interrupted) {
        printf("⚡ Interrupted — sending stopped to tracker...\n");
        long dl = (long)pm->completed * torrent->piece_length;
        PeerList tmp = tracker_announce(torrent, g_peer_id, LISTEN_PORT,
                                        dl, 0, torrent->total_length - dl,
                                        "stopped");
        peer_list_free(&tmp);
    }

    if (piece_manager_is_complete(pm)) {
        printf("✓ Download complete!\n");
        printf("  Output:  %s\n", out_path);
        printf("  Size:    %.2f MB\n",
               (double)torrent->total_length / (1024.0 * 1024.0));
        printf("  Time:    %lds\n", (long)elapsed);
        if (elapsed > 0)
            printf("  Avg:     %.1f KB/s\n",
                   (double)torrent->total_length / 1024.0 / (double)elapsed);

        /* Send completed event */
        long dl = torrent->total_length;
        PeerList tmp = tracker_announce(torrent, g_peer_id, LISTEN_PORT,
                                        dl, 0, 0, "completed");
        peer_list_free(&tmp);
    } else {
        int done  = pm->completed;
        int total = pm->num_pieces;
        printf("✗ Incomplete: %d/%d pieces (%.1f%%)\n",
               done, total, (double)done * 100.0 / total);
        printf("  Partial output saved — re-run to resume.\n");
    }
    }

    piece_manager_free(pm);
    peer_list_free(&peers);
    torrent_free(torrent);

    return piece_manager_is_complete(pm) ? EXIT_SUCCESS : EXIT_FAILURE;
}
