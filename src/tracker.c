/**
 * tracker.c — HTTP Tracker Communication
 *
 * Builds the announce URL, makes an HTTP GET request using libcurl,
 * then parses the bencoded response to extract peer addresses.
 *
 * See docs/04_tracker.md for a full explanation.
 */

#include "../include/tracker.h"
#include "../include/bencode.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>

/* ── libcurl write callback ──────────────────────────────────────────────── */

/*
 * libcurl calls this function each time it receives data.
 * We accumulate all chunks into a dynamically-growing buffer.
 *
 * This is the standard "write to memory" pattern for libcurl.
 */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   capacity;
} CurlBuffer;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuffer *buf  = (CurlBuffer *)userdata;
    size_t      bytes = size * nmemb;

    /* Grow buffer if needed */
    if (buf->len + bytes > buf->capacity) {
        buf->capacity = (buf->len + bytes) * 2;
        buf->data     = realloc(buf->data, buf->capacity);
        if (!buf->data) return 0; /* signal error to libcurl */
    }

    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    return bytes;
}

/* ── generate_peer_id ────────────────────────────────────────────────────── */

void generate_peer_id(uint8_t *out) {
    /*
     * Peer ID format (BEP 0020 "Azureus-style"):
     *
     *   -BT0001-xxxxxxxxxxxx
     *   ^^^^^^^^             → client identifier + version (8 bytes)
     *           ^^^^^^^^^^^^  → 12 random alphanumeric bytes
     *
     * Total: 20 bytes (NOT null-terminated — it's a raw byte string)
     *
     * -BT = our fake "BTorrent" client code
     * 0001 = version 0.0.0.1
     */
    const char *prefix = "-BT0001-";
    memcpy(out, prefix, 8);

    /* Fill remaining 12 bytes with random alphanumeric characters */
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789";

    srand((unsigned int)time(NULL));
    for (int i = 8; i < 20; i++) {
        out[i] = charset[rand() % (sizeof(charset) - 1)];
    }
}

/* ── Parse compact peer list ─────────────────────────────────────────────── */

static PeerList parse_peers_compact(const uint8_t *data, size_t len) {
    /*
     * Compact format: each peer is exactly 6 bytes:
     *   [IP0][IP1][IP2][IP3][PORT_HIGH][PORT_LOW]
     *
     * All in big-endian (network) byte order.
     *
     * Example: C0 A8 01 01 1A E1
     *   = IP: 192.168.1.1, Port: 6881 (0x1AE1)
     */
    PeerList pl;
    pl.count    = 0;
    pl.interval = 1800; /* default 30 minutes */

    if (len % 6 != 0) {
        fprintf(stderr, "tracker: compact peers length %zu is not a multiple of 6\n", len);
        pl.peers = NULL;
        return pl;
    }

    int count  = (int)(len / 6);
    pl.peers   = xcalloc(count, sizeof(Peer));
    pl.count   = count;

    for (int i = 0; i < count; i++) {
        const uint8_t *p = data + (i * 6);

        /* IPv4 address: 4 bytes */
        snprintf(pl.peers[i].ip, sizeof(pl.peers[i].ip),
                 "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);

        /* Port: 2 bytes big-endian */
        pl.peers[i].port = read_uint16_be(p + 4);
    }

    return pl;
}

/* ── Parse dictionary peer list ──────────────────────────────────────────── */

static PeerList parse_peers_dict(BencodeNode *peers_node) {
    /*
     * Dictionary format: a list of dicts, each with keys:
     *   "ip"      → string IPv4 address
     *   "port"    → integer port
     *   "peer id" → 20-byte peer ID (optional)
     */
    PeerList pl;
    pl.count    = 0;
    pl.interval = 1800;
    pl.peers    = NULL;

    if (peers_node->type != BENCODE_LIST) return pl;

    int count = (int)peers_node->list.count;
    pl.peers  = xcalloc(count, sizeof(Peer));

    for (int i = 0; i < count; i++) {
        BencodeNode *entry = peers_node->list.items[i];
        if (entry->type != BENCODE_DICT) continue;

        BencodeNode *ip   = bencode_dict_get(entry, "ip");
        BencodeNode *port = bencode_dict_get(entry, "port");

        if (!ip || ip->type != BENCODE_STR) continue;
        if (!port || port->type != BENCODE_INT) continue;

        size_t ip_len = ip->str.len < 15 ? ip->str.len : 15;
        memcpy(pl.peers[pl.count].ip, ip->str.data, ip_len);
        pl.peers[pl.count].ip[ip_len] = '\0';
        pl.peers[pl.count].port       = (uint16_t)port->integer;
        pl.count++;
    }

    return pl;
}

/* ── tracker_announce ────────────────────────────────────────────────────── */

PeerList tracker_announce(const TorrentInfo *torrent,
                          const uint8_t     *peer_id,
                          uint16_t           port,
                          long               downloaded,
                          long               uploaded,
                          long               left,
                          const char        *event) {

    PeerList empty = { .peers = NULL, .count = 0, .interval = 1800 };

    /* Step 1: URL-encode the info_hash (20 raw bytes → percent-encoded) */
    char encoded_hash[61]; /* 20 bytes × 3 chars + null */
    url_encode_bytes(torrent->info_hash, 20, encoded_hash);

    /* Step 2: URL-encode the peer_id */
    char encoded_peer_id[61];
    url_encode_bytes(peer_id, 20, encoded_peer_id);

    /* Step 3: Build the full announce URL */
    char url[2048];
    int url_len = snprintf(url, sizeof(url),
        "%s"
        "?info_hash=%s"
        "&peer_id=%s"
        "&port=%d"
        "&uploaded=%ld"
        "&downloaded=%ld"
        "&left=%ld"
        "&compact=1",   /* Request compact peer list (6 bytes/peer) */
        torrent->announce,
        encoded_hash,
        encoded_peer_id,
        port,
        uploaded,
        downloaded,
        left);

    /* Append event if provided */
    if (event && event[0] != '\0') {
        snprintf(url + url_len, sizeof(url) - url_len,
                 "&event=%s", event);
    }

    printf("tracker: announcing to %s\n", torrent->announce);

    /* Step 4: Make HTTP GET request using libcurl */
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "tracker: curl_easy_init failed\n");
        return empty;
    }

    CurlBuffer response = { .data = NULL, .len = 0, .capacity = 0 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);      /* 30 second timeout */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); /* follow redirects */

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "tracker: HTTP request failed: %s\n",
                curl_easy_strerror(res));
        free(response.data);
        return empty;
    }

    if (!response.data || response.len == 0) {
        fprintf(stderr, "tracker: empty response\n");
        return empty;
    }

    /* Step 5: Parse the bencoded response */
    BencodeNode *root = bencode_parse(response.data, response.len);
    if (!root) {
        fprintf(stderr, "tracker: failed to parse response\n");
        free(response.data);
        return empty;
    }

    /* Check for failure reason */
    BencodeNode *failure = bencode_dict_get(root, "failure reason");
    if (failure && failure->type == BENCODE_STR) {
        fprintf(stderr, "tracker: failure: %.*s\n",
                (int)failure->str.len, failure->str.data);
        bencode_free(root);
        free(response.data);
        return empty;
    }

    /* Extract interval */
    int interval = 1800;
    BencodeNode *interval_node = bencode_dict_get(root, "interval");
    if (interval_node && interval_node->type == BENCODE_INT) {
        interval = (int)interval_node->integer;
    }

    /* Step 6: Extract peer list */
    BencodeNode *peers_node = bencode_dict_get(root, "peers");
    if (!peers_node) {
        fprintf(stderr, "tracker: no 'peers' in response\n");
        bencode_free(root);
        free(response.data);
        return empty;
    }

    PeerList pl;
    if (peers_node->type == BENCODE_STR) {
        /* Compact format (most common) */
        pl = parse_peers_compact(peers_node->str.data, peers_node->str.len);
    } else if (peers_node->type == BENCODE_LIST) {
        /* Dictionary format (older trackers) */
        pl = parse_peers_dict(peers_node);
    } else {
        fprintf(stderr, "tracker: unknown peers format\n");
        pl = empty;
    }

    pl.interval = interval;

    printf("tracker: got %d peers (re-announce in %d seconds)\n",
           pl.count, pl.interval);

    bencode_free(root);
    free(response.data);
    return pl;
}

/* ── peer_list_free ──────────────────────────────────────────────────────── */

void peer_list_free(PeerList *pl) {
    if (!pl) return;
    free(pl->peers);
    pl->peers = NULL;
    pl->count = 0;
}
