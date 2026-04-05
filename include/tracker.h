/**
 * tracker.h — HTTP Tracker Communication
 *
 * Handles the announce request to a BitTorrent tracker:
 *   1. Build the announce URL with all required query parameters
 *   2. Make an HTTP GET request (using libcurl)
 *   3. Parse the bencoded response
 *   4. Return a list of peer IP:port pairs
 *
 * See docs/04_tracker.md for full protocol explanation.
 *
 * Usage:
 *   PeerList peers = tracker_announce(torrent, peer_id, 6881);
 *   for (int i = 0; i < peers.count; i++) {
 *       printf("Peer: %s:%d\n", peers.peers[i].ip, peers.peers[i].port);
 *   }
 *   peer_list_free(&peers);
 */

#ifndef BTORRENT_TRACKER_H
#define BTORRENT_TRACKER_H

#include "torrent.h"
#include <stdint.h>

/* ── Peer address ────────────────────────────────────────────────────────── */

/**
 * Peer - a peer's IP address and port number.
 * This is all we need to connect to them via TCP.
 */
typedef struct {
    char     ip[16];    /* null-terminated IPv4 address, e.g. "1.2.3.4" */
    uint16_t port;      /* TCP port number */
} Peer;

/* ── Peer list returned by tracker ──────────────────────────────────────── */

typedef struct {
    Peer  *peers;   /* heap-allocated array of Peer structs */
    int    count;   /* number of peers */
    int    interval; /* suggested re-announce interval in seconds */
} PeerList;

/* ── Tracker announce ────────────────────────────────────────────────────── */

/**
 * tracker_announce - send an announce request to the tracker.
 *
 * @torrent    the torrent we're announcing for
 * @peer_id    our 20-byte peer ID
 * @port       our listening port (for other peers to connect to us)
 * @downloaded bytes downloaded so far
 * @uploaded   bytes uploaded so far
 * @left       bytes remaining
 * @event      "started", "stopped", "completed", or "" (empty = regular update)
 *
 * @return PeerList with count > 0 on success, count = 0 on failure.
 *
 * Internally:
 *   1. URL-encodes info_hash and peer_id
 *   2. Builds the full announce URL with query parameters
 *   3. Uses libcurl to GET the URL
 *   4. Parses the bencoded response
 *   5. Decodes compact peer list (6 bytes per peer: 4 IP + 2 port)
 *
 * The returned PeerList must be freed with peer_list_free().
 */
PeerList tracker_announce(const TorrentInfo *torrent,
                          const uint8_t     *peer_id,
                          uint16_t           port,
                          long               downloaded,
                          long               uploaded,
                          long               left,
                          const char        *event);

/**
 * peer_list_free - free a PeerList returned by tracker_announce.
 */
void peer_list_free(PeerList *pl);

/**
 * generate_peer_id - generate a random 20-byte peer ID.
 *
 * Format: "-BT0001-" followed by 12 random alphanumeric characters.
 * See BEP 0020 for peer ID conventions.
 *
 * @out  20-byte output buffer (NOT null-terminated)
 */
void generate_peer_id(uint8_t *out);

#endif /* BTORRENT_TRACKER_H */
