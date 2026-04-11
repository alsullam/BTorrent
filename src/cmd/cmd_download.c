#define _POSIX_C_SOURCE 200809L
/**
 * cmd_download.c — "-d" download mode
 *
 * Owns the full download lifecycle:
 *   1. Parse torrent
 *   2. Announce to tracker (with retry + backoff)
 *   3. Create piece manager (handles resume automatically)
 *   4. Hand off to scheduler_run() — epoll concurrent peer download
 *   5. Send "completed" or "stopped" tracker event on finish/interrupt
 */

#include "cmd/cmd.h"
#include "scheduler.h"
#include "core/torrent.h"
#include "proto/tracker.h"
#include "proto/peer.h"
#include "core/pieces.h"
#include "utils.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

/* Signal flag declared in main.c, shared via extern */
extern volatile sig_atomic_t g_interrupted;

/* ── cmd_download ────────────────────────────────────────────────────────── */

int cmd_download(const Config *cfg) {
    time_t session_start = time(NULL);

    /* 1. Parse torrent */
    LOG_INFO("[1/4] Parsing: %s", cfg->torrent_path);
    TorrentInfo *torrent = torrent_parse(cfg->torrent_path);
    if (!torrent) return EXIT_FAILURE;
    torrent_print(torrent);

    /* 2. Resolve output path.
     *    -o sets the DIRECTORY to save into (matching qBittorrent/Transmission).
     *    For single-file torrents: output = out_dir/torrent_name
     *    For multi-file torrents:  output = out_dir/torrent_name/ (directory)
     *    If -o is not given, default to torrent name in current directory. */
    char out_path[1024];
    if (cfg->output_path[0]) {
        int n = snprintf(out_path, sizeof(out_path) - 1, "%s/%s",
                         cfg->output_path, torrent->name);
        if (n < 0 || n >= (int)sizeof(out_path) - 1)
            strncpy(out_path, torrent->name, sizeof(out_path) - 1);
    } else {
        strncpy(out_path, torrent->name, sizeof(out_path) - 1);
    }
    out_path[sizeof(out_path) - 1] = '\0';

    /* 3. Peer ID + tracker announce */
    uint8_t peer_id[20];
    generate_peer_id(peer_id);

    LOG_INFO("%s", "[2/4] Announcing to tracker...");
    PeerList peers = tracker_announce_with_retry(
        torrent, peer_id, cfg->port,
        0, 0, torrent->total_length, "started");
    if (peers.count == 0) {
        LOG_ERROR("%s", "No peers received from any tracker");
        torrent_free(torrent);
        return EXIT_FAILURE;
    }
    LOG_INFO("      %d peers (re-announce in %ds)",
             peers.count, peers.interval);

    /* If the primary tracker returned very few peers, scrape backup trackers
     * right now to build a larger pool before starting the scheduler. */
    if (peers.count < 50 && torrent->num_trackers > 0) {
        LOG_INFO("%s", "      few peers — trying backup trackers...");
        for (int i = 0; i < torrent->num_trackers; i++) {
            const char *url = torrent->announce_list[i];
            if (url[0] == '\0' || strcmp(url, torrent->announce) == 0) continue;
            PeerList extra = tracker_announce_url(url, torrent, peer_id, cfg->port,
                0, 0, torrent->total_length, "started");
            if (extra.count > 0) {
                /* Merge, deduplicating by IP:port */
                int added = 0;
                for (int j = 0; j < extra.count; j++) {
                    int dup = 0;
                    for (int k = 0; k < peers.count; k++) {
                        if (strcmp(peers.peers[k].ip, extra.peers[j].ip) == 0 &&
                            peers.peers[k].port == extra.peers[j].port) {
                            dup = 1; break;
                        }
                    }
                    if (!dup) {
                        peers.peers = realloc(peers.peers,
                            (size_t)(peers.count + 1) * sizeof(Peer));
                        peers.peers[peers.count++] = extra.peers[j];
                        added++;
                    }
                }
                LOG_INFO("      +%d peers (deduped) from %s", added, url);
                peer_list_free(&extra);
                if (peers.count >= 200) break;
            }
        }
        LOG_INFO("      total: %d unique peers", peers.count);
    }

    /* 4. Piece manager (auto-resumes from existing output) */
    LOG_INFO("[3/4] Piece manager → %s", out_path);
    PieceManager *pm = piece_manager_new(torrent, out_path);
    if (!pm) {
        peer_list_free(&peers);
        torrent_free(torrent);
        return EXIT_FAILURE;
    }

    if (piece_manager_is_complete(pm)) {
        LOG_INFO("%s", "All pieces already on disk — nothing to download.");
        piece_manager_free(pm);
        peer_list_free(&peers);
        torrent_free(torrent);
        return EXIT_SUCCESS;
    }

    /* 5. Download — concurrent epoll scheduler */
    LOG_INFO("[4/4] Downloading %d pieces via up to %d peers...",
             torrent->num_pieces, cfg->max_peers);

    int rc = scheduler_run(torrent, pm, &peers, peer_id, cfg, &g_interrupted);

    /* 6. Tracker completion / stopped event */
    time_t elapsed = time(NULL) - session_start;

    if (g_interrupted) {
        LOG_INFO("%s", "Interrupted — sending stopped to tracker");
        long dl = (long)pm->completed * torrent->piece_length;
        PeerList tmp = tracker_announce(torrent, peer_id, cfg->port,
                                        dl, 0, torrent->total_length - dl,
                                        "stopped");
        peer_list_free(&tmp);
    }

    int complete = piece_manager_is_complete(pm);

    if (complete) {
        LOG_INFO("%s", "Download complete!");
        LOG_INFO("  Output : %s", out_path);
        LOG_INFO("  Size   : %.2f MB",
                 (double)torrent->total_length / (1024.0 * 1024.0));
        LOG_INFO("  Time   : %lds", (long)elapsed);
        if (elapsed > 0)
            LOG_INFO("  Avg    : %.1f KB/s",
                     (double)torrent->total_length / 1024.0 / (double)elapsed);

        long dl = torrent->total_length;
        PeerList tmp = tracker_announce(torrent, peer_id, cfg->port,
                                        dl, 0, 0, "completed");
        peer_list_free(&tmp);
    } else {
        LOG_WARN("Incomplete: %d/%d pieces — re-run to resume",
                 pm->completed, pm->num_pieces);
    }

    piece_manager_free(pm);
    peer_list_free(&peers);
    torrent_free(torrent);
    (void)rc;
    return complete ? EXIT_SUCCESS : EXIT_FAILURE;
}
