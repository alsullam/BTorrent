#define _POSIX_C_SOURCE 200809L
/**
 * scheduler.c — Concurrent peer download scheduler (epoll-based)
 *
 * BUG FIX (major): the original nb_recv approach would return EAGAIN
 * mid-message and lose state. Each session now has an internal read buffer
 * so partial reads are accumulated across epoll events. A full message is
 * only processed when every byte has arrived.
 *
 * State machine:
 *   CONNECTING → HANDSHAKE → BITFIELD_WAIT → INTERESTED → DOWNLOADING
 *              ↑                                               |
 *              └───────────── IDLE (re-unchoked) ─────────────┘
 *   Any state → DEAD (error / peer closed)
 */

#include "scheduler.h"
#include "net/tcp.h"
#include "proto/peer.h"
#include "proto/tracker.h"
#include "core/pieces.h"
#include "utils.h"
#include "log.h"
#include "result.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <limits.h>

/* ── Per-session read buffer ─────────────────────────────────────────────── */

#define RBUF_SIZE (1024 * 1024)   /* 1 MiB — fits any piece block + header */

typedef struct {
    uint8_t *data;
    size_t   len;     /* bytes currently in buffer */
    size_t   cap;     /* allocated size (RBUF_SIZE) */
} ReadBuf;

static void rbuf_init(ReadBuf *b) {
    b->data = xmalloc(RBUF_SIZE);
    b->len  = 0;
    b->cap  = RBUF_SIZE;
}

static void rbuf_free(ReadBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
}

/* Drain as many bytes as available from sock into buffer. Returns -1 on error. */
static int rbuf_fill(ReadBuf *b, int sock) {
    while (b->len < b->cap) {
        ssize_t n = recv(sock, b->data + b->len, b->cap - b->len, 0);
        if (n > 0) { b->len += (size_t)n; continue; }
        if (n == 0) return -1;   /* peer closed */
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* done for now */
        return -1;
    }
    return 0;
}

/* Consume `need` bytes from the front of the buffer. */
static int rbuf_consume(ReadBuf *b, size_t need, uint8_t *dst) {
    if (b->len < need) return 0;   /* not enough yet */
    if (dst) memcpy(dst, b->data, need);
    memmove(b->data, b->data + need, b->len - need);
    b->len -= need;
    return 1;   /* consumed */
}

/* ── Peer state machine ──────────────────────────────────────────────────── */

typedef enum {
    PS_CONNECTING,
    PS_HANDSHAKE,
    PS_BITFIELD_WAIT,
    PS_INTERESTED,
    PS_DOWNLOADING,
    PS_IDLE,
    PS_DEAD,
} PeerPhase;

typedef struct {
    int        sock;
    PeerPhase  phase;
    char       ip[16];
    uint16_t   port;

    uint8_t    peer_id[20];
    int        am_choked;

    uint8_t   *peer_bitfield;
    int        bf_len;

    int        piece_idx;
    int        piece_len;
    int        num_blocks;
    int        blocks_sent;
    int        blocks_recv;

    time_t     last_active;
    time_t     last_keepalive;
    int        fail_count;

    ReadBuf    rbuf;
} Session;

/* ── Handshake ───────────────────────────────────────────────────────────── */

#define HANDSHAKE_LEN 68
#define PSTR          "BitTorrent protocol"
#define PSTRLEN       19

static void build_handshake(uint8_t *buf,
                             const uint8_t *info_hash,
                             const uint8_t *peer_id) {
    buf[0] = PSTRLEN;
    memcpy(buf + 1,  PSTR,      PSTRLEN);
    memset(buf + 20, 0,         8);
    memcpy(buf + 28, info_hash, 20);
    memcpy(buf + 48, peer_id,   20);
}

static int verify_handshake(const uint8_t *buf, const uint8_t *info_hash) {
    if (buf[0] != PSTRLEN)                         return -1;
    if (memcmp(buf + 1,  PSTR,      PSTRLEN) != 0) return -1;
    if (memcmp(buf + 28, info_hash, 20)      != 0) return -1;
    return 0;
}

/* ── Non-blocking send ───────────────────────────────────────────────────── */

static int nb_send(int sock, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    int retries = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0)  { sent += (size_t)n; retries = 0; continue; }
        if (n == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (++retries > 50) return -1;
            struct timespec ts = { 0, 100000 };
            nanosleep(&ts, NULL);
            continue;
        }
        return -1;
    }
    return 0;
}

/* ── Rarest-first piece selection ────────────────────────────────────────── */

static int next_rarest(PieceManager *pm,
                        Session *sessions, int max_s,
                        const uint8_t *peer_bf, int num_pieces) {
    int *avail = xcalloc((size_t)pm->num_pieces, sizeof(int));
    for (int s = 0; s < max_s; s++) {
        if (sessions[s].phase == PS_DEAD || !sessions[s].peer_bitfield) continue;
        for (int i = 0; i < pm->num_pieces; i++)
            if (bitfield_has_piece(sessions[s].peer_bitfield, i))
                avail[i]++;
    }
    int best = -1, best_n = INT_MAX;
    for (int i = 0; i < pm->num_pieces; i++) {
        if (pm->pieces[i].state != PIECE_EMPTY) continue;
        if (peer_bf && i < num_pieces && !bitfield_has_piece(peer_bf, i)) continue;
        if (avail[i] > 0 && avail[i] < best_n) { best = i; best_n = avail[i]; }
    }
    if (best == -1 && peer_bf) {
        for (int i = 0; i < pm->num_pieces; i++) {
            if (pm->pieces[i].state != PIECE_EMPTY) continue;
            if (i < num_pieces && bitfield_has_piece(peer_bf, i)) { best = i; break; }
        }
    }
    free(avail);
    return best;
}

/* ── MSG_HAVE sender ─────────────────────────────────────────────────────── */

static void send_have_msg(int sock, int piece_idx) {
    uint8_t buf[9];
    write_uint32_be(buf,     5);
    buf[4] = MSG_HAVE;
    write_uint32_be(buf + 5, (uint32_t)piece_idx);
    send(sock, buf, 9, MSG_NOSIGNAL);
}

static void send_keepalive(int sock) {
    uint8_t buf[4] = {0, 0, 0, 0};
    send(sock, buf, 4, MSG_NOSIGNAL);
}

/* ── Session lifecycle ───────────────────────────────────────────────────── */

static void session_init(Session *s, int sock, const char *ip, uint16_t port) {
    s->sock           = sock;
    s->phase          = PS_CONNECTING;
    s->piece_idx      = -1;
    s->am_choked      = 1;
    s->last_active    = time(NULL);
    s->last_keepalive = time(NULL);
    s->fail_count     = 0;
    s->blocks_sent    = 0;
    s->blocks_recv    = 0;
    s->peer_bitfield  = NULL;
    s->bf_len         = 0;
    memset(s->peer_id, 0, 20);
    strncpy(s->ip, ip, 15);
    s->ip[15] = '\0';
    s->port    = port;
    rbuf_init(&s->rbuf);
}

static void session_close(Session *s, int epfd) {
    if (s->sock >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, s->sock, NULL);
        close(s->sock);
        s->sock = -1;
    }
    free(s->peer_bitfield);
    s->peer_bitfield = NULL;
    rbuf_free(&s->rbuf);
    s->phase = PS_DEAD;
}

/* ── epoll helpers ───────────────────────────────────────────────────────── */

static void epoll_watch(int epfd, int fd, uint32_t ev, int idx) {
    struct epoll_event e = { .events = ev, .data.u32 = (uint32_t)idx };
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
}

/* ── Pipeline: send as many block REQUESTs as the depth allows ───────────── */

static int send_requests(Session *s, const Config *cfg) {
    while (s->blocks_sent < s->num_blocks &&
           s->blocks_sent - s->blocks_recv < cfg->pipeline_depth) {
        int beg  = s->blocks_sent * BLOCK_SIZE;
        int blen = (beg + BLOCK_SIZE > s->piece_len)
                   ? s->piece_len - beg : BLOCK_SIZE;
        PeerConn tmp = { .sock = s->sock };
        if (peer_send_request(&tmp, (uint32_t)s->piece_idx,
                              (uint32_t)beg, (uint32_t)blen) < 0)
            return -1;
        s->blocks_sent++;
    }
    return 0;
}

/* ── Assign a piece and start the pipeline ───────────────────────────────── */

static void assign_piece(Session *s, PieceManager *pm,
                          const TorrentInfo *torrent,
                          Session *all, int max_s,
                          const Config *cfg) {
    if (s->phase != PS_DOWNLOADING || s->am_choked || s->piece_idx >= 0) return;

    int pi = next_rarest(pm, all, max_s, s->peer_bitfield, torrent->num_pieces);
    if (pi < 0) { s->phase = PS_IDLE; return; }

    s->piece_idx   = pi;
    s->piece_len   = torrent_get_piece_length(torrent, pi);
    s->num_blocks  = (s->piece_len + BLOCK_SIZE - 1) / BLOCK_SIZE;
    s->blocks_sent = 0;
    s->blocks_recv = 0;
    pm->pieces[pi].state = PIECE_ASSIGNED;

    LOG_DEBUG("sched: %s:%d → piece %d/%d",
              s->ip, s->port, pi, torrent->num_pieces - 1);

    if (send_requests(s, cfg) < 0)
        s->phase = PS_DEAD;
}

/* ── Return a piece in progress back to the pool ─────────────────────────── */

static void return_piece(Session *s, PieceManager *pm) {
    if (s->piece_idx < 0) return;
    int pi = s->piece_idx;
    if (pm->pieces[pi].state == PIECE_ACTIVE) {
        free(pm->pieces[pi].data);
        pm->pieces[pi].data = NULL;
        pm->pieces[pi].state = PIECE_EMPTY;
        memset(pm->pieces[pi].block_received, 0,
               (size_t)pm->pieces[pi].num_blocks);
        pm->pieces[pi].blocks_done = 0;
    } else if (pm->pieces[pi].state == PIECE_ASSIGNED) {
        pm->pieces[pi].state = PIECE_EMPTY;
    }
    s->piece_idx   = -1;
    s->blocks_sent = 0;
    s->blocks_recv = 0;
}

/* ── Message dispatcher ──────────────────────────────────────────────────── */

static void dispatch_msg(Session *s, uint8_t id, uint8_t *payload, uint32_t plen,
                          int epfd, int idx,
                          const TorrentInfo *torrent, PieceManager *pm,
                          Session *all, int max_s, const Config *cfg) {
    int bf_bytes = (torrent->num_pieces + 7) / 8;
    (void)epfd; (void)idx;

    switch (id) {

    case MSG_CHOKE:
        s->am_choked = 1;
        return_piece(s, pm);
        s->phase = PS_IDLE;
        break;

    case MSG_UNCHOKE:
        s->am_choked = 0;
        if (s->phase == PS_INTERESTED || s->phase == PS_IDLE ||
            s->phase == PS_BITFIELD_WAIT)
            s->phase = PS_DOWNLOADING;
        break;

    case MSG_HAVE: {
        if (plen != 4) { s->phase = PS_DEAD; break; }
        uint32_t pi = read_uint32_be(payload);
        if ((int)pi < torrent->num_pieces && s->peer_bitfield)
            bitfield_set_piece(s->peer_bitfield, (int)pi);
        if (s->phase == PS_IDLE && !s->am_choked)
            s->phase = PS_DOWNLOADING;
        break;
    }

    case MSG_BITFIELD: {
        if (!s->peer_bitfield)
            s->peer_bitfield = xcalloc((size_t)bf_bytes, 1);
        s->bf_len = bf_bytes;
        uint32_t copy = plen < (uint32_t)bf_bytes ? plen : (uint32_t)bf_bytes;
        memcpy(s->peer_bitfield, payload, copy);
        LOG_INFO("peer %s:%d: got BITFIELD", s->ip, s->port);
        break;
    }

    case MSG_PIECE: {
        if (plen < 8) { s->phase = PS_DEAD; break; }
        int pi    = (int)read_uint32_be(payload);
        int begin = (int)read_uint32_be(payload + 4);
        int dlen  = (int)(plen - 8);

        int result = piece_manager_on_block(pm, pi, begin, payload + 8, dlen);

        if (pi == s->piece_idx) s->blocks_recv++;

        if (result == 1) {
            /* Piece complete */
            send_have_msg(s->sock, pi);
            s->piece_idx   = -1;
            s->blocks_sent = 0;
            s->blocks_recv = 0;
            s->phase       = PS_DOWNLOADING;
        } else if (result == -1) {
            /* SHA-1 fail */
            s->piece_idx   = -1;
            s->blocks_sent = 0;
            s->blocks_recv = 0;
        } else {
            /* Refill pipeline */
            if (send_requests(s, cfg) < 0)
                s->phase = PS_DEAD;
        }
        break;
    }

    default:
        break;
    }

    (void)all; (void)max_s;
}

/* ── handle_session ──────────────────────────────────────────────────────── */

static void handle_session(Session *s, uint32_t ev_flags,
                            int epfd, int idx,
                            const TorrentInfo *torrent,
                            PieceManager *pm,
                            const uint8_t *info_hash,
                            const uint8_t *our_peer_id,
                            Session *all, int max_s,
                            const Config *cfg) {
    s->last_active = time(NULL);
    int bf_bytes   = (torrent->num_pieces + 7) / 8;

    if (s->phase == PS_CONNECTING) {
        if (!(ev_flags & EPOLLOUT)) {
            session_close(s, epfd); return;
        }
        if (tcp_finish_connect(s->sock) < 0) {
            LOG_INFO("peer %s:%d: connect failed", s->ip, s->port);
            session_close(s, epfd); return;
        }
        tcp_set_timeouts(s->sock, cfg->peer_timeout_s);
        uint8_t hs[HANDSHAKE_LEN];
        build_handshake(hs, info_hash, our_peer_id);
        if (nb_send(s->sock, hs, HANDSHAKE_LEN) < 0) {
            LOG_INFO("peer %s:%d: handshake send failed", s->ip, s->port);
            session_close(s, epfd); return;
        }
        LOG_INFO("peer %s:%d: connected OK", s->ip, s->port);
        s->phase         = PS_HANDSHAKE;
        s->peer_bitfield = xcalloc((size_t)bf_bytes, 1);
        s->bf_len        = bf_bytes;
        epoll_watch(epfd, s->sock, EPOLLIN | EPOLLET, idx);
        return;
    }

    if (s->phase == PS_HANDSHAKE) {
        if (rbuf_fill(&s->rbuf, s->sock) < 0) {
            LOG_INFO("peer %s:%d: disconnected during handshake", s->ip, s->port);
            session_close(s, epfd); return;
        }
        uint8_t their_hs[HANDSHAKE_LEN];
        if (!rbuf_consume(&s->rbuf, HANDSHAKE_LEN, their_hs)) return;
        if (verify_handshake(their_hs, info_hash) < 0) {
            LOG_INFO("peer %s:%d: bad handshake", s->ip, s->port);
            session_close(s, epfd); return;
        }
        memcpy(s->peer_id, their_hs + 48, 20);
        LOG_INFO("peer %s:%d: handshake OK", s->ip, s->port);

        if (pm->completed > 0) {
            uint8_t *bfmsg = xmalloc((size_t)(5 + pm->bf_len));
            write_uint32_be(bfmsg, (uint32_t)(1 + pm->bf_len));
            bfmsg[4] = MSG_BITFIELD;
            memcpy(bfmsg + 5, pm->our_bitfield, (size_t)pm->bf_len);
            nb_send(s->sock, bfmsg, (size_t)(5 + pm->bf_len));
            free(bfmsg);
        }

        /* Send INTERESTED immediately — BEP3: BITFIELD is optional */
        uint8_t interested_msg[5];
        write_uint32_be(interested_msg, 1);
        interested_msg[4] = MSG_INTERESTED;
        if (nb_send(s->sock, interested_msg, 5) < 0) {
            session_close(s, epfd); return;
        }
        s->am_choked = 1;
        s->phase     = PS_INTERESTED;
        return;
    }

    if (!(ev_flags & EPOLLIN)) return;
    if (rbuf_fill(&s->rbuf, s->sock) < 0) {
        LOG_INFO("peer %s:%d: disconnected (phase=%d)", s->ip, s->port, (int)s->phase);
        session_close(s, epfd); return;
    }

    while (s->phase != PS_DEAD) {
        if (s->rbuf.len < 4) break;

        uint32_t msg_len = read_uint32_be(s->rbuf.data);
        if (msg_len == 0) {
            rbuf_consume(&s->rbuf, 4, NULL);   /* keepalive */
            continue;
        }
        if (msg_len > 16 * 1024 * 1024) { session_close(s, epfd); return; }
        if (s->rbuf.len < 4 + msg_len) break;

        rbuf_consume(&s->rbuf, 4, NULL);
        uint8_t id = 0;
        rbuf_consume(&s->rbuf, 1, &id);

        uint32_t plen = msg_len - 1;
        uint8_t *payload = NULL;
        if (plen > 0) {
            payload = xmalloc(plen);
            rbuf_consume(&s->rbuf, plen, payload);
        }

        dispatch_msg(s, id, payload, plen, epfd, idx,
                     torrent, pm, all, max_s, cfg);
        free(payload);

        if ((s->phase == PS_DOWNLOADING) && !s->am_choked && s->piece_idx < 0)
            assign_piece(s, pm, torrent, all, max_s, cfg);
    }

    (void)bf_bytes;
}

/* ── open_connection ─────────────────────────────────────────────────────── */

static int open_connection(Session *s, int epfd, int idx,
                            const char *ip, uint16_t port) {
    int sock = tcp_connect_nb(ip, port);
    if (sock < 0) return -1;
    session_init(s, sock, ip, port);
    struct epoll_event ev = { .events  = EPOLLOUT | EPOLLET,
                              .data.u32 = (uint32_t)idx };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) < 0) {
        close(sock); s->sock = -1;
        rbuf_free(&s->rbuf);
        return -1;
    }
    return 0;
}

/* ── scheduler_run ───────────────────────────────────────────────────────── */

int scheduler_run(const TorrentInfo *torrent,
                  PieceManager      *pm,
                  PeerList          *peers,
                  const uint8_t     *peer_id,
                  const Config      *cfg,
                  volatile sig_atomic_t *interrupted) {

    int max_s = cfg->max_peers > 0 ? cfg->max_peers : 50;

    Session *sessions = xcalloc((size_t)max_s, sizeof(Session));
    for (int i = 0; i < max_s; i++) {
        sessions[i].sock      = -1;
        sessions[i].phase     = PS_DEAD;
        sessions[i].rbuf.data = NULL;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { free(sessions); return EXIT_FAILURE; }

    int    peer_cursor   = 0;
    int    active        = 0;
    time_t last_announce = time(NULL);
    int    announce_int  = peers->interval > 0 ? peers->interval : 1800;
    time_t last_progress = time(NULL);

    for (int i = 0; i < max_s && peer_cursor < peers->count; i++) {
        const Peer *p = &peers->peers[peer_cursor++];
        if (open_connection(&sessions[i], epfd, i, p->ip, p->port) == 0)
            active++;
    }
    LOG_INFO("sched: %d connections opened (max %d peers)", active, max_s);

    struct epoll_event events[64];

    while (!piece_manager_is_complete(pm) && !(*interrupted)) {

        /* Re-announce */
        if (time(NULL) - last_announce >= announce_int) {
            long dl = (long)pm->completed * torrent->piece_length;
            LOG_INFO("%s", "sched: re-announcing...");
            PeerList np = tracker_announce_with_retry(
                torrent, peer_id, cfg->port,
                dl, 0, torrent->total_length - dl, "");
            if (np.count > 0) {
                peer_list_free(peers);
                *peers       = np;
                peer_cursor  = 0;
                announce_int = np.interval > 0 ? np.interval : 1800;
                LOG_INFO("sched: %d fresh peers", np.count);
            }
            last_announce = time(NULL);
        }

        /* Assign pieces */
        for (int i = 0; i < max_s; i++) {
            Session *s = &sessions[i];
            if (s->phase == PS_DOWNLOADING && !s->am_choked && s->piece_idx < 0)
                assign_piece(s, pm, torrent, sessions, max_s, cfg);
        }

        /* Keepalive: send every 90s to prevent peer timeout (BEP3: 2 min limit) */
        time_t now_ka = time(NULL);
        for (int i = 0; i < max_s; i++) {
            Session *s = &sessions[i];
            if (s->sock < 0 || s->phase == PS_DEAD || s->phase == PS_CONNECTING)
                continue;
            if (now_ka - s->last_keepalive >= 90) {
                send_keepalive(s->sock);
                s->last_keepalive = now_ka;
            }
        }

        int n = epoll_wait(epfd, events, 64, 200);
        if (n < 0 && errno == EINTR) continue;

        for (int e = 0; e < n; e++) {
            int idx = (int)events[e].data.u32;
            if (idx < 0 || idx >= max_s) continue;
            Session *s = &sessions[idx];
            if (s->phase == PS_DEAD || s->sock < 0) continue;

            if (events[e].events & (EPOLLERR | EPOLLHUP)) {
                return_piece(s, pm);
                session_close(s, epfd);
                continue;
            }

            handle_session(s, events[e].events, epfd, idx,
                           torrent, pm, torrent->info_hash, peer_id,
                           sessions, max_s, cfg);

            if (s->phase == PS_DOWNLOADING && !s->am_choked
                && s->piece_idx < 0 && s->sock >= 0)
                assign_piece(s, pm, torrent, sessions, max_s, cfg);
        }

        /* Housekeeping */
        time_t now_hk = time(NULL);
        int dead = 0;
        for (int i = 0; i < max_s; i++) {
            Session *s = &sessions[i];
            if (s->phase != PS_DEAD && s->phase != PS_CONNECTING &&
                s->sock >= 0 && cfg->peer_timeout_s > 0 &&
                (now_hk - s->last_active) > (cfg->peer_timeout_s * 3)) {
                LOG_DEBUG("sched: %s:%d timed out", s->ip, s->port);
                return_piece(s, pm);
                session_close(s, epfd);
            }
            if (s->phase != PS_DEAD) continue;
            dead++;
            return_piece(s, pm);
            if (peer_cursor < peers->count) {
                const Peer *p = &peers->peers[peer_cursor++];
                if (open_connection(&sessions[i], epfd, i, p->ip, p->port) == 0)
                    dead--;
            }
        }
        active = max_s - dead;

        if (time(NULL) != last_progress) {
            last_progress = time(NULL);
            piece_manager_print_progress(pm);
        }

        if (active <= 0 && peer_cursor >= peers->count) {
            time_t now        = time(NULL);
            int    wait_cap   = (peers->count < 10) ? 30 : 120;
            int    wait_secs  = (int)(announce_int - (now - last_announce));
            if (wait_secs > wait_cap) wait_secs = wait_cap;
            if (wait_secs > 0) {
                LOG_INFO("sched: all %d peers tried — re-announcing in %ds",
                         peers->count, wait_secs);
                sleep((unsigned int)wait_secs);
            }
            last_announce = 0;
        }
    }

    for (int i = 0; i < max_s; i++) {
        if (sessions[i].sock >= 0) session_close(&sessions[i], epfd);
        else if (sessions[i].rbuf.data) rbuf_free(&sessions[i].rbuf);
    }
    close(epfd);
    free(sessions);
    return piece_manager_is_complete(pm) ? EXIT_SUCCESS : EXIT_FAILURE;
}
