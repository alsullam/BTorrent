/**
 * tracker.c — HTTP + UDP Tracker Communication (BEP 3 + BEP 15)
 *
 * Implements:
 *   - HTTP announce (BEP 3): GET request with bencoded response
 *   - UDP  announce (BEP 15): connectionless binary protocol
 *   - Multi-tracker fallback (BEP 12): try backup trackers on failure
 *
 * UDP is preferred by most modern torrents — it's faster and more
 * efficient. The protocol uses a two-step exchange:
 *   1. Connect request  → connection_id (valid 60 seconds)
 *   2. Announce request → peer list
 */

#define _POSIX_C_SOURCE 200809L
#include "tracker.h"
#include "bencode.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <curl/curl.h>

/* libcurl response buffer */

typedef struct { uint8_t *data; size_t len; size_t cap; } CurlBuf;

static size_t curl_wcb(void *ptr, size_t sz, size_t n, void *ud) {
    CurlBuf *b = (CurlBuf *)ud;
    size_t bytes = sz * n;
    if (b->len + bytes > b->cap) {
        b->cap = (b->len + bytes) * 2 + 512;
        b->data = realloc(b->data, b->cap);
        if (!b->data) return 0;
    }
    memcpy(b->data + b->len, ptr, bytes);
    b->len += bytes;
    return bytes;
}

/* Byte helpers */

static void  u32be(uint8_t *b, uint32_t v) {
    b[0]=(v>>24)&0xFF; b[1]=(v>>16)&0xFF; b[2]=(v>>8)&0xFF; b[3]=v&0xFF;
}
static void  u64be(uint8_t *b, uint64_t v) {
    u32be(b, (uint32_t)(v>>32)); u32be(b+4, (uint32_t)v);
}
static uint32_t r32be(const uint8_t *b) {
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
    ((uint32_t)b[2]<<8)|(uint32_t)b[3];
}

/* generate_peer_id */

void generate_peer_id(uint8_t *out) {
    const char *prefix = "-BT0001-";
    memcpy(out, prefix, 8);
    static const char cs[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    srand((unsigned int)(ts.tv_nsec ^ (unsigned long)ts.tv_sec));
    for (int i = 8; i < 20; i++)
        out[i] = cs[rand() % (sizeof(cs) - 1)];
}

/* Compact peer parsers */

static PeerList compact_peers(const uint8_t *d, size_t len) {
    PeerList pl = {NULL, 0, 1800};
    if (len % 6 != 0) return pl;
    int cnt = (int)(len / 6);
    pl.peers = xcalloc(cnt, sizeof(Peer));
    pl.count = cnt;
    for (int i = 0; i < cnt; i++) {
        const uint8_t *p = d + i*6;
        snprintf(pl.peers[i].ip, 16, "%d.%d.%d.%d", p[0],p[1],p[2],p[3]);
        pl.peers[i].port = read_uint16_be(p + 4);
    }
    return pl;
}

static PeerList dict_peers(BencodeNode *n) {
    PeerList pl = {NULL, 0, 1800};
    if (!n || n->type != BENCODE_LIST) return pl;
    int cnt = (int)n->list.count;
    pl.peers = xcalloc(cnt, sizeof(Peer));
    for (int i = 0; i < cnt; i++) {
        BencodeNode *e = n->list.items[i];
        if (e->type != BENCODE_DICT) continue;
        BencodeNode *ip   = bencode_dict_get(e, "ip");
        BencodeNode *port = bencode_dict_get(e, "port");
        if (!ip || ip->type != BENCODE_STR) continue;
        if (!port || port->type != BENCODE_INT) continue;
        size_t l = ip->str.len < 15 ? ip->str.len : 15;
        memcpy(pl.peers[pl.count].ip, ip->str.data, l);
        pl.peers[pl.count].port = (uint16_t)port->integer;
        pl.count++;
    }
    return pl;
}

/* HTTP announce (BEP 3) */

static PeerList http_announce(const char *base,
                              const TorrentInfo *t, const uint8_t *pid,
                              uint16_t port, long dl, long ul, long left,
                              const char *event) {
    PeerList empty = {NULL, 0, 1800};
    char eh[61], ep[61];
    url_encode_bytes(t->info_hash, 20, eh);
    url_encode_bytes(pid, 20, ep);

    char url[2048];
    int n = snprintf(url, sizeof(url),
                     "%s?info_hash=%s&peer_id=%s&port=%d"
                     "&uploaded=%ld&downloaded=%ld&left=%ld&compact=1",
                     base, eh, ep, port, ul, dl, left);
    if (event && event[0])
        snprintf(url+n, sizeof(url)-n, "&event=%s", event);

    CURL *c = curl_easy_init();
    if (!c) return empty;
    CurlBuf buf = {NULL, 0, 0};
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,     "BTorrent/1.0");
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK || !buf.data) { free(buf.data); return empty; }

    BencodeNode *root = bencode_parse(buf.data, buf.len);
    free(buf.data);
    if (!root) return empty;

    BencodeNode *fail = bencode_dict_get(root, "failure reason");
    if (fail && fail->type == BENCODE_STR) {
        fprintf(stderr, "tracker HTTP failure: %.*s\n",
                (int)fail->str.len, fail->str.data);
        bencode_free(root); return empty;
    }

    int interval = 1800;
    BencodeNode *iv = bencode_dict_get(root, "interval");
    if (iv && iv->type == BENCODE_INT) interval = (int)iv->integer;

    BencodeNode *pn = bencode_dict_get(root, "peers");
    PeerList pl = {NULL, 0, 1800};
    if (pn) {
        if (pn->type == BENCODE_STR)
            pl = compact_peers(pn->str.data, pn->str.len);
        else
            pl = dict_peers(pn);
    }
    pl.interval = interval;
    bencode_free(root);
    return pl;
}

/* UDP announce (BEP 15) */

static int parse_udp_url(const char *url, char *host, size_t hlen, int *port) {
    const char *p = url;
    if (strncmp(p, "udp://", 6) == 0) p += 6;
    else return -1;
    const char *colon = strrchr(p, ':');
    if (!colon) return -1;
    size_t hl = (size_t)(colon - p);
    if (hl >= hlen) hl = hlen - 1;
    memcpy(host, p, hl); host[hl] = '\0';
    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535) return -1;
    return 0;
}

static PeerList udp_announce(const char *url,
                             const TorrentInfo *t, const uint8_t *pid,
                             uint16_t port, long dl, long ul, long left,
                             const char *event) {
    PeerList empty = {NULL, 0, 1800};
    char host[256]; int tport;
    if (parse_udp_url(url, host, sizeof(host), &tport) < 0) return empty;

    /* Resolve hostname */
    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char pstr[8]; snprintf(pstr, sizeof(pstr), "%d", tport);
    if (getaddrinfo(host, pstr, &hints, &res) != 0) {
        fprintf(stderr, "tracker UDP: DNS failed for %s\n", host);
        return empty;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { freeaddrinfo(res); return empty; }

    struct timeval tv = {15, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in *saddr = (struct sockaddr_in *)res->ai_addr;
    socklen_t slen = sizeof(*saddr);

    /* Step 1: Connect */
    uint32_t txid = (uint32_t)rand();
    uint8_t  req[16];
    u64be(req,     0x41727101980ULL);  /* magic: BEP 15 connect magic */
    u32be(req+8,   0);                  /* action = 0 (connect) */
    u32be(req+12,  txid);
    if (sendto(sock, req, 16, 0, (struct sockaddr*)saddr, slen) != 16)
        goto fail;

    uint8_t resp[16];
    if (recv(sock, resp, sizeof(resp), 0) < 16) goto fail;
    if (r32be(resp) != 0 || r32be(resp+4) != txid) goto fail;

    uint64_t conn_id = ((uint64_t)r32be(resp+8)<<32) | r32be(resp+12);

    /* Step 2: Announce */
    uint32_t ev_code = 0;
    if (event) {
        if      (strcmp(event,"started")   == 0) ev_code = 2;
        else if (strcmp(event,"stopped")   == 0) ev_code = 3;
        else if (strcmp(event,"completed") == 0) ev_code = 1;
    }

    txid = (uint32_t)rand();
    uint8_t areq[98];
    u64be(areq,    conn_id);
    u32be(areq+8,  1);                 /* action = 1 (announce) */
    u32be(areq+12, txid);
    memcpy(areq+16, t->info_hash, 20);
    memcpy(areq+36, pid, 20);
    u64be(areq+56, (uint64_t)dl);
    u64be(areq+64, (uint64_t)left);
    u64be(areq+72, (uint64_t)ul);
    u32be(areq+80, ev_code);
    u32be(areq+84, 0);                 /* ip = 0 */
    u32be(areq+88, (uint32_t)rand());  /* key */
    u32be(areq+92, (uint32_t)-1);      /* num_want = default */
    areq[96] = (port >> 8) & 0xFF;
    areq[97] = port & 0xFF;

    if (sendto(sock, areq, 98, 0, (struct sockaddr*)saddr, slen) != 98)
        goto fail;

    uint8_t aresp[4096];
    ssize_t rlen = recv(sock, aresp, sizeof(aresp), 0);
    close(sock); freeaddrinfo(res);

    if (rlen < 20) return empty;
    if (r32be(aresp) != 1 || r32be(aresp+4) != txid) return empty;

    int interval = (int)r32be(aresp+8);
    printf("tracker UDP: seeders=%u leechers=%u interval=%d\n",
           r32be(aresp+16), r32be(aresp+12), interval);

    PeerList pl = compact_peers(aresp+20, (size_t)(rlen-20));
    pl.interval = interval;
    return pl;

fail:
    close(sock); freeaddrinfo(res);
    fprintf(stderr, "tracker UDP: failed for %s\n", url);
    return empty;
}

/* try_one_tracker */

static PeerList try_tracker(const char *url,
                            const TorrentInfo *t, const uint8_t *pid,
                            uint16_t port, long dl, long ul, long left,
                            const char *event) {
    PeerList empty = {NULL, 0, 1800};
    if (!url || !url[0]) return empty;
    printf("tracker: trying %s\n", url);
    if (strncmp(url, "udp://", 6) == 0)
        return udp_announce(url, t, pid, port, dl, ul, left, event);
    if (strncmp(url, "http://",  7) == 0 ||
        strncmp(url, "https://", 8) == 0)
        return http_announce(url, t, pid, port, dl, ul, left, event);
    fprintf(stderr, "tracker: unsupported scheme in: %s\n", url);
    return empty;
}

/* tracker_announce */

PeerList tracker_announce(const TorrentInfo *torrent,
                          const uint8_t     *peer_id,
                          uint16_t           port,
                          long               downloaded,
                          long               uploaded,
                          long               left,
                          const char        *event) {
    /*
     * Multi-tracker fallback (BEP 12):
     *   1. Try primary announce URL
     *   2. On failure, iterate announce-list tiers in order
     *   3. Return first successful peer list
     */
    if (torrent->announce[0]) {
        PeerList pl = try_tracker(torrent->announce, torrent, peer_id,
                                  port, downloaded, uploaded, left, event);
        if (pl.count > 0) {
            printf("tracker: %d peers from primary\n", pl.count);
            return pl;
        }
    }

    for (int i = 0; i < torrent->num_trackers; i++) {
        const char *url = torrent->announce_list[i];
        if (strcmp(url, torrent->announce) == 0) continue;  /* skip primary */

        PeerList pl = try_tracker(url, torrent, peer_id,
                                  port, downloaded, uploaded, left, event);
        if (pl.count > 0) {
            printf("tracker: %d peers from backup: %s\n", pl.count, url);
            return pl;
        }
    }

    fprintf(stderr, "tracker: all trackers exhausted\n");
    return (PeerList){NULL, 0, 1800};
}

/* peer_list_free */

void peer_list_free(PeerList *pl) {
    if (!pl) return;
    free(pl->peers);
    pl->peers = NULL;
    pl->count = 0;
}
