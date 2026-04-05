/**
 * main.c — Entry Point & Download Orchestration
 *
 * This is where everything comes together:
 *
 *   1. Parse the .torrent file → TorrentInfo
 *   2. Announce to tracker    → PeerList
 *   3. Connect to peers       → PeerConn
 *   4. Download pieces        → PieceManager
 *   5. Write to disk          → output file
 *
 * The main download loop:
 *   - Try each peer in the list
 *   - Handshake, express interest, wait for unchoke
 *   - Request all blocks of each piece
 *   - Receive pieces and verify SHA-1
 *   - Move to next peer when this one is exhausted or fails
 *
 * This is a simple sequential (one peer at a time) implementation.
 * A real client downloads from multiple peers simultaneously using
 * threads or non-blocking I/O (select/epoll).
 *
 * See README.md for the recommended learning order.
 */

#include "../include/torrent.h"
#include "../include/tracker.h"
#include "../include/peer.h"
#include "../include/pieces.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────────────────── */

#define LISTEN_PORT     6881   /* Our (fake) listening port */
#define MAX_RETRIES     3      /* Times to retry a failed piece */
#define PIPELINE_DEPTH  5      /* Requests to send before waiting for reply */

/* ── download_from_peer ──────────────────────────────────────────────────── */

/**
 * download_from_peer - attempt to download as much as possible from one peer.
 *
 * This function:
 *   1. Connects to the peer
 *   2. Exchanges handshake
 *   3. Reads the bitfield (which pieces they have)
 *   4. Sends interested + waits for unchoke
 *   5. Downloads pieces by requesting blocks
 *   6. Returns when the peer disconnects or we're done
 *
 * @torrent     parsed torrent metadata
 * @pm          piece manager (tracks what we have/need)
 * @peer        peer address from tracker
 * @info_hash   20-byte info hash for handshake
 * @our_peer_id our 20-byte peer ID
 *
 * @return number of pieces successfully downloaded from this peer
 */
static int download_from_peer(const TorrentInfo *torrent,
                              PieceManager      *pm,
                              const Peer        *peer,
                              const uint8_t     *info_hash,
                              const uint8_t     *our_peer_id) {

    int pieces_downloaded = 0;

    /* Step 1: Connect via TCP */
    PeerConn *conn = peer_connect(peer->ip, peer->port, info_hash, our_peer_id);
    if (!conn) return 0;  /* connection refused or timed out — common */

    /* Step 2: Handshake */
    if (peer_handshake(conn, info_hash, our_peer_id) < 0) {
        peer_close(conn);
        return 0;
    }

    /*
     * Step 3: Receive the bitfield.
     *
     * After the handshake, the peer should send a BITFIELD message telling
     * us which pieces they have. This is optional — if they have no pieces
     * they may skip it. We handle both cases.
     */
    uint8_t *peer_bitfield = NULL;
    PeerMsg  msg;

    /* Read messages until we get something actionable */
    while (1) {
        if (peer_recv_msg(conn, &msg) < 0) {
            fprintf(stderr, "peer %s:%d: error receiving message\n",
                    peer->ip, peer->port);
            peer_close(conn);
            free(peer_bitfield);
            return pieces_downloaded;
        }

        if (msg.type == MSG_BITFIELD) {
            peer_bitfield = msg.bitfield;
            msg.bitfield  = NULL;   /* take ownership */
            printf("peer %s:%d: got bitfield (%u bytes)\n",
                   peer->ip, peer->port, msg.bitfield_len);
            break;
        } else if (msg.type == MSG_HAVE) {
            /* Some peers send HAVE instead of BITFIELD */
            /* For simplicity, skip these pre-bitfield HAVEs */
            peer_msg_free(&msg);
        } else if (msg.type == MSG_UNCHOKE) {
            /* Peer unchoked us before bitfield — unusual but handle it */
            conn->am_choked = 0;
            peer_msg_free(&msg);
            break;
        } else {
            peer_msg_free(&msg);
            /* Keep reading — might be a keepalive or other harmless msg */
        }
    }

    /* Step 4: Express interest and wait to be unchoked */
    peer_send_interested(conn);

    if (conn->am_choked) {
        /* Wait for unchoke (with a limit on messages we'll tolerate) */
        int attempts = 0;
        while (conn->am_choked && attempts < 20) {
            if (peer_recv_msg(conn, &msg) < 0) break;
            if (msg.type == MSG_UNCHOKE) {
                printf("peer %s:%d: unchoked!\n", peer->ip, peer->port);
            }
            peer_msg_free(&msg);
            attempts++;
        }
    }

    if (conn->am_choked) {
        printf("peer %s:%d: never unchoked us, skipping\n",
               peer->ip, peer->port);
        free(peer_bitfield);
        peer_close(conn);
        return pieces_downloaded;
    }

    /*
     * Step 5: Download pieces.
     *
     * For each piece we need (that this peer has):
     *   a. Request each BLOCK_SIZE block within the piece
     *   b. Receive piece messages until the full piece arrives
     *   c. SHA-1 verify the piece (done inside piece_manager_on_block)
     *   d. Write to disk (done inside piece_manager_on_block)
     *
     * PIPELINING: We send PIPELINE_DEPTH requests before reading replies.
     * This keeps the network pipe full and avoids the round-trip delay
     * of sending one request and waiting for one reply.
     */
    while (!piece_manager_is_complete(pm)) {
        int piece_idx = piece_manager_next_needed(pm, peer_bitfield,
                                                  torrent->num_pieces);
        if (piece_idx < 0) {
            printf("peer %s:%d: no more pieces available from this peer\n",
                   peer->ip, peer->port);
            break;
        }

        int piece_len   = torrent_get_piece_length(torrent, piece_idx);
        int num_blocks  = (piece_len + BLOCK_SIZE - 1) / BLOCK_SIZE;

        printf("peer %s:%d: downloading piece %d/%d (%d blocks)\n",
               peer->ip, peer->port,
               piece_idx, torrent->num_pieces - 1,
               num_blocks);

        /* Send requests for all blocks in this piece */
        int blocks_requested = 0;
        int blocks_received  = 0;

        /*
         * Pipeline: send up to PIPELINE_DEPTH requests, then start
         * reading replies. As each reply comes in, send the next request.
         */
        int send_block = 0;  /* next block index to request */

        /* Seed the pipeline */
        while (send_block < num_blocks && blocks_requested < PIPELINE_DEPTH) {
            int begin = send_block * BLOCK_SIZE;
            int blen  = BLOCK_SIZE;
            /* Last block may be smaller */
            if (begin + blen > piece_len) blen = piece_len - begin;

            if (peer_send_request(conn, piece_idx, begin, blen) < 0) {
                fprintf(stderr, "peer %s:%d: send request failed\n",
                        peer->ip, peer->port);
                goto done_with_peer;
            }
            send_block++;
            blocks_requested++;
        }

        /* Receive replies, sending new requests as we go */
        int piece_failed = 0;
        while (blocks_received < num_blocks) {
            if (peer_recv_msg(conn, &msg) < 0) {
                fprintf(stderr, "peer %s:%d: recv failed\n",
                        peer->ip, peer->port);
                piece_failed = 1;
                break;
            }

            switch (msg.type) {
                case MSG_PIECE:
                    if ((int)msg.piece_index != piece_idx) {
                        /* Got a block for a different piece (from pipelining) */
                        /* Just feed it to the piece manager anyway */
                        piece_manager_on_block(pm,
                            (int)msg.piece_index,
                            (int)msg.piece_begin,
                            msg.piece_data,
                            (int)msg.piece_data_len);
                        peer_msg_free(&msg);
                        break;
                    }

                    /* Feed the block to the piece manager */
                    {
                        int result = piece_manager_on_block(pm,
                            (int)msg.piece_index,
                            (int)msg.piece_begin,
                            msg.piece_data,
                            (int)msg.piece_data_len);

                        peer_msg_free(&msg);
                        blocks_received++;

                        /* Send next request to keep pipeline full */
                        if (send_block < num_blocks) {
                            int begin = send_block * BLOCK_SIZE;
                            int blen  = BLOCK_SIZE;
                            if (begin + blen > piece_len)
                                blen = piece_len - begin;

                            peer_send_request(conn, piece_idx, begin, blen);
                            send_block++;
                        }

                        if (result == 1) {
                            /* Piece complete and verified */
                            pieces_downloaded++;
                        } else if (result == -1) {
                            /* SHA-1 failure — piece reset, will retry */
                            piece_failed = 1;
                        }
                    }
                    break;

                case MSG_CHOKE:
                    /* Peer re-choked us mid-download */
                    printf("peer %s:%d: choked mid-download!\n",
                           peer->ip, peer->port);
                    piece_failed = 1;
                    break;

                case MSG_HAVE:
                    /* Peer finished another piece — ignore for now */
                    peer_msg_free(&msg);
                    break;

                case MSG_KEEPALIVE:
                case MSG_UNCHOKE:
                    peer_msg_free(&msg);
                    break;

                default:
                    peer_msg_free(&msg);
                    break;
            }

            if (piece_failed) break;
        }

        if (piece_failed) break;
    }

done_with_peer:
    free(peer_bitfield);
    peer_close(conn);
    return pieces_downloaded;
}

/* ── print_usage ─────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "BTorrent — Educational BitTorrent Client\n"
        "Usage: %s <file.torrent> [output_path]\n"
        "\n"
        "  file.torrent  Path to the .torrent file\n"
        "  output_path   Where to save the download (default: torrent name)\n"
        "\n"
        "Example:\n"
        "  %s ubuntu.torrent\n"
        "  %s ubuntu.torrent /tmp/ubuntu.iso\n",
        prog, prog, prog);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *torrent_path = argv[1];

    /* ── Step 1: Parse the torrent file ─────────────────────────────────── */

    printf("=== BTorrent — Educational BitTorrent Client ===\n\n");
    printf("[1/4] Parsing torrent file: %s\n", torrent_path);

    TorrentInfo *torrent = torrent_parse(torrent_path);
    if (!torrent) {
        fprintf(stderr, "Failed to parse torrent file\n");
        return EXIT_FAILURE;
    }
    torrent_print(torrent);

    /* Determine output path */
    char out_path[1024];
    if (argc >= 3) {
        strncpy(out_path, argv[2], sizeof(out_path) - 1);
    } else {
        strncpy(out_path, torrent->name, sizeof(out_path) - 1);
    }

    /* ── Step 2: Generate peer ID and announce to tracker ───────────────── */

    uint8_t peer_id[20];
    generate_peer_id(peer_id);

    printf("\n[2/4] Announcing to tracker: %s\n", torrent->announce);

    PeerList peers = tracker_announce(torrent, peer_id, LISTEN_PORT,
                                      0, 0, torrent->total_length, "started");

    if (peers.count == 0) {
        fprintf(stderr, "No peers received from tracker\n");
        torrent_free(torrent);
        return EXIT_FAILURE;
    }

    printf("      Got %d peers\n", peers.count);

    /* ── Step 3: Initialize piece manager ───────────────────────────────── */

    printf("\n[3/4] Initializing piece manager → %s\n", out_path);

    PieceManager *pm = piece_manager_new(torrent, out_path);
    if (!pm) {
        fprintf(stderr, "Failed to initialize piece manager\n");
        peer_list_free(&peers);
        torrent_free(torrent);
        return EXIT_FAILURE;
    }

    /* ── Step 4: Download loop ───────────────────────────────────────────── */

    printf("\n[4/4] Downloading (%d pieces, %d bytes each)...\n\n",
           torrent->num_pieces, torrent->piece_length);

    time_t start_time = time(NULL);
    int    total_downloaded = 0;

    /*
     * Main download loop: try each peer in turn.
     *
     * A production client would:
     * - Connect to multiple peers simultaneously (threading/epoll)
     * - Use "rarest first" piece selection
     * - Implement tit-for-tat choking
     * - Re-announce to tracker periodically
     *
     * We keep it simple: one peer at a time, sequential pieces.
     */
    for (int i = 0; i < peers.count && !piece_manager_is_complete(pm); i++) {
        printf("Trying peer [%d/%d]: %s:%d\n",
               i + 1, peers.count, peers.peers[i].ip, peers.peers[i].port);

        int n = download_from_peer(torrent, pm, &peers.peers[i],
                                   torrent->info_hash, peer_id);
        total_downloaded += n;

        if (n > 0) {
            printf("  → Got %d pieces from this peer\n", n);
        }
    }

    /* ── Results ─────────────────────────────────────────────────────────── */

    time_t elapsed = time(NULL) - start_time;
    printf("\n=== Download Complete ===\n");

    if (piece_manager_is_complete(pm)) {
        printf("✓ All %d pieces downloaded and verified!\n", torrent->num_pieces);
        printf("  File: %s\n", out_path);
        printf("  Size: %.2f MB\n",
               (double)torrent->total_length / (1024.0 * 1024.0));
        printf("  Time: %ld seconds\n", (long)elapsed);
        if (elapsed > 0) {
            printf("  Speed: %.1f KB/s avg\n",
                   (double)torrent->total_length / (1024.0 * (double)elapsed));
        }

        /* Notify tracker that we're done */
        PeerList done_peers = tracker_announce(torrent, peer_id, LISTEN_PORT,
                                               torrent->total_length, 0, 0,
                                               "completed");
        peer_list_free(&done_peers);

    } else {
        int done = pm->completed;
        int total = pm->num_pieces;
        fprintf(stderr,
                "✗ Incomplete: %d/%d pieces downloaded (%.1f%%)\n"
                "  Ran out of peers. Try again — more peers may be available.\n",
                done, total, (double)done * 100.0 / total);
    }

    /* ── Cleanup ─────────────────────────────────────────────────────────── */

    piece_manager_free(pm);
    peer_list_free(&peers);
    torrent_free(torrent);

    return piece_manager_is_complete(pm) ? EXIT_SUCCESS : EXIT_FAILURE;
}
