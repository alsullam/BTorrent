/**
 * peer.c — Peer Wire Protocol Implementation
 *
 * Implements the full BitTorrent peer wire protocol:
 *   - TCP connection to a peer
 *   - 68-byte handshake exchange
 *   - Message framing (4-byte length prefix + 1-byte ID)
 *   - Sending: interested, request
 *   - Receiving and parsing: bitfield, have, unchoke, piece
 *
 * See docs/05_peer_protocol.md for full explanation.
 */

#include "../include/peer.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Constants ───────────────────────────────────────────────────────────── */

#define HANDSHAKE_SIZE    68   /* 1 + 19 + 8 + 20 + 20 */
#define PSTR              "BitTorrent protocol"
#define PSTRLEN           19

/* ── Internal: reliable send ─────────────────────────────────────────────── */

/*
 * send_all - keep sending until all bytes are written.
 *
 * send() may return less than len if the socket buffer is full.
 * We loop until everything is sent or an error occurs.
 */
static int send_all(int sock, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0) perror("send");
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* ── Internal: reliable receive ──────────────────────────────────────────── */

/*
 * recv_all - keep receiving until exactly `len` bytes are read.
 *
 * recv() may return less than len (partial reads are normal on TCP).
 * MSG_WAITALL is a hint, but we loop to be safe.
 */
static int recv_all(int sock, uint8_t *buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(sock, buf + received, len - received, 0);
        if (n <= 0) {
            if (n < 0 && errno != EINTR) perror("recv");
            return -1;
        }
        received += (size_t)n;
    }
    return 0;
}

/* ── peer_connect ────────────────────────────────────────────────────────── */

PeerConn *peer_connect(const char    *ip,
                       uint16_t       port,
                       const uint8_t *info_hash,
                       const uint8_t *peer_id) {
    (void)info_hash; /* stored for handshake — passed to peer_handshake() */
    (void)peer_id;

    /* Step 1: Create a TCP socket
     *
     * AF_INET     = IPv4
     * SOCK_STREAM = TCP (reliable, ordered byte stream)
     * 0           = default protocol for SOCK_STREAM = TCP
     */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return NULL;
    }

    /* Step 2: Set a connection timeout (5 seconds)
     *
     * Without this, connect() may hang for minutes on a dead peer.
     * SO_SNDTIMEO sets the timeout for send operations including connect.
     */
    struct timeval timeout;
    timeout.tv_sec  = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* Step 3: Fill in the server address structure */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);  /* htons: host-to-network byte order */

    /*
     * inet_pton: parse an IPv4 address string into binary.
     * "pton" = "presentation to network"
     */
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "peer: invalid IP address: %s\n", ip);
        close(sock);
        return NULL;
    }

    /* Step 4: Connect! */
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* This is common — many peers in the list are dead or unreachable */
        close(sock);
        return NULL;
    }

    /* Step 5: Allocate and initialize the connection struct */
    PeerConn *conn = xcalloc(1, sizeof(PeerConn));
    conn->sock         = sock;
    conn->am_choked    = 1; /* start choked — peer must unchoke us */
    conn->am_interested = 0;
    strncpy(conn->ip, ip, sizeof(conn->ip) - 1);
    conn->port = port;

    return conn;
}

/* ── peer_close ──────────────────────────────────────────────────────────── */

void peer_close(PeerConn *conn) {
    if (!conn) return;
    close(conn->sock);
    free(conn);
}

/* ── peer_handshake ──────────────────────────────────────────────────────── */

int peer_handshake(PeerConn *conn, const uint8_t *info_hash,
                   const uint8_t *our_peer_id) {
    /*
     * The handshake is a fixed 68-byte exchange:
     *
     * Offset  Size  Description
     * ------  ----  -----------
     *   0      1    pstrlen = 19
     *   1     19    pstr = "BitTorrent protocol"
     *  20      8    reserved (all zeros, for extension bits)
     *  28     20    info_hash
     *  48     20    peer_id
     *
     * We send first, then receive and verify theirs.
     */

    /* Build our handshake */
    uint8_t our_hs[HANDSHAKE_SIZE];
    our_hs[0] = PSTRLEN;                       /* 1 byte: 19 */
    memcpy(our_hs + 1,  PSTR, PSTRLEN);        /* 19 bytes: protocol string */
    memset(our_hs + 20, 0, 8);                 /* 8 bytes: reserved (zeros) */
    memcpy(our_hs + 28, info_hash, 20);        /* 20 bytes: info_hash */
    memcpy(our_hs + 48, our_peer_id, 20);      /* 20 bytes: our peer_id */

    /* Send our handshake */
    if (send_all(conn->sock, our_hs, HANDSHAKE_SIZE) < 0) {
        fprintf(stderr, "peer %s:%d: failed to send handshake\n",
                conn->ip, conn->port);
        return -1;
    }

    /* Receive their handshake */
    uint8_t their_hs[HANDSHAKE_SIZE];
    if (recv_all(conn->sock, their_hs, HANDSHAKE_SIZE) < 0) {
        fprintf(stderr, "peer %s:%d: failed to receive handshake\n",
                conn->ip, conn->port);
        return -1;
    }

    /* Verify pstrlen */
    if (their_hs[0] != PSTRLEN) {
        fprintf(stderr, "peer %s:%d: bad pstrlen %d\n",
                conn->ip, conn->port, their_hs[0]);
        return -1;
    }

    /* Verify protocol string */
    if (memcmp(their_hs + 1, PSTR, PSTRLEN) != 0) {
        fprintf(stderr, "peer %s:%d: wrong protocol string\n",
                conn->ip, conn->port);
        return -1;
    }

    /* Verify info_hash — critical! Must match or we're on different torrents */
    if (memcmp(their_hs + 28, info_hash, 20) != 0) {
        fprintf(stderr, "peer %s:%d: info_hash mismatch!\n",
                conn->ip, conn->port);
        return -1;
    }

    /* Save their peer_id (for logging/debugging) */
    memcpy(conn->peer_id, their_hs + 48, 20);

    printf("peer %s:%d: handshake OK\n", conn->ip, conn->port);
    return 0;
}

/* ── Internal: send a simple no-payload message ──────────────────────────── */

static int send_simple_msg(PeerConn *conn, uint8_t msg_id) {
    /*
     * Simple messages (choke, unchoke, interested, uninterested) have:
     *   [00 00 00 01] [msg_id]
     *   ^^^^^^^^^^^^  ^^^^^^
     *   length = 1    the message type
     */
    uint8_t buf[5];
    write_uint32_be(buf, 1);  /* length = 1 (just the ID byte) */
    buf[4] = msg_id;
    return send_all(conn->sock, buf, 5);
}

/* ── peer_send_interested ────────────────────────────────────────────────── */

int peer_send_interested(PeerConn *conn) {
    printf("peer %s:%d: sending interested\n", conn->ip, conn->port);
    conn->am_interested = 1;
    return send_simple_msg(conn, MSG_INTERESTED);
}

/* ── peer_send_request ───────────────────────────────────────────────────── */

int peer_send_request(PeerConn *conn, uint32_t index,
                      uint32_t begin, uint32_t length) {
    /*
     * REQUEST message format:
     *   [length=13][id=6][index 4B][begin 4B][length 4B]
     *
     * Total: 4 + 1 + 4 + 4 + 4 = 17 bytes
     *
     * All integers are big-endian (network byte order).
     *
     * index  = which piece
     * begin  = byte offset within the piece
     * length = how many bytes to send (usually BLOCK_SIZE = 16384)
     */
    uint8_t buf[17];
    write_uint32_be(buf,     13);      /* payload length: 1 + 4 + 4 + 4 = 13 */
    buf[4] = MSG_REQUEST;
    write_uint32_be(buf + 5,  index);
    write_uint32_be(buf + 9,  begin);
    write_uint32_be(buf + 13, length);

    return send_all(conn->sock, buf, 17);
}

/* ── peer_recv_msg ───────────────────────────────────────────────────────── */

int peer_recv_msg(PeerConn *conn, PeerMsg *msg) {
    /*
     * All messages after the handshake share this framing:
     *
     *   [length 4B][id 1B][payload variable]
     *
     * Exception: keep-alive has length=0 (no ID byte).
     *
     * We:
     *   1. Read the 4-byte length
     *   2. If length=0, it's a keep-alive
     *   3. Otherwise, read 1 byte for the ID
     *   4. Read `length - 1` remaining bytes as payload
     *   5. Parse the payload based on the ID
     */

    memset(msg, 0, sizeof(PeerMsg));

    /* Step 1: Read length prefix */
    uint8_t len_buf[4];
    if (recv_all(conn->sock, len_buf, 4) < 0) {
        return -1;
    }
    uint32_t msg_len = read_uint32_be(len_buf);

    /* Step 2: Keep-alive */
    if (msg_len == 0) {
        msg->type = MSG_KEEPALIVE;
        return 0;
    }

    /* Guard against absurdly large messages (e.g. 16 MiB max) */
    if (msg_len > 16 * 1024 * 1024) {
        fprintf(stderr, "peer %s:%d: message too large: %u bytes\n",
                conn->ip, conn->port, msg_len);
        return -1;
    }

    /* Step 3: Read message ID */
    uint8_t id;
    if (recv_all(conn->sock, &id, 1) < 0) {
        return -1;
    }
    msg->type = (MsgType)id;

    uint32_t payload_len = msg_len - 1;

    /* Step 4 & 5: Read and parse payload based on message type */
    switch (id) {

        case MSG_CHOKE:
        case MSG_UNCHOKE:
        case MSG_INTERESTED:
        case MSG_UNINTERESTED:
            /* No payload */
            if (id == MSG_CHOKE)   conn->am_choked = 1;
            if (id == MSG_UNCHOKE) conn->am_choked = 0;
            break;

        case MSG_HAVE:
            /*
             * HAVE: 4-byte piece index.
             * Peer is announcing they just finished downloading a piece.
             */
            if (payload_len != 4) return -1;
            {
                uint8_t buf[4];
                if (recv_all(conn->sock, buf, 4) < 0) return -1;
                msg->have_index = read_uint32_be(buf);
            }
            break;

        case MSG_BITFIELD:
            /*
             * BITFIELD: N bytes where bit i (MSB first) = peer has piece i.
             *
             * This is usually the first message after the handshake.
             * We heap-allocate the bitfield bytes.
             */
            msg->bitfield     = xmalloc(payload_len);
            msg->bitfield_len = payload_len;
            if (recv_all(conn->sock, msg->bitfield, payload_len) < 0) {
                free(msg->bitfield);
                msg->bitfield = NULL;
                return -1;
            }
            break;

        case MSG_REQUEST:
        case MSG_CANCEL:
            /*
             * REQUEST/CANCEL: 12 bytes (index, begin, length).
             * We receive these when acting as a seeder (sending data).
             * For our downloader-only client, we just consume and ignore.
             */
            {
                uint8_t buf[12];
                if (recv_all(conn->sock, buf, 12) < 0) return -1;
                /* Ignored in this downloader-only implementation */
            }
            break;

        case MSG_PIECE:
            /*
             * PIECE: 8-byte header + block data
             *
             * [index 4B][begin 4B][block data ...]
             *
             * This is the data we actually want!
             * We heap-allocate the block data.
             */
            if (payload_len < 8) return -1;
            {
                uint8_t header[8];
                if (recv_all(conn->sock, header, 8) < 0) return -1;

                msg->piece_index    = read_uint32_be(header);
                msg->piece_begin    = read_uint32_be(header + 4);
                msg->piece_data_len = payload_len - 8;
                msg->piece_data     = xmalloc(msg->piece_data_len);

                if (recv_all(conn->sock, msg->piece_data, msg->piece_data_len) < 0) {
                    free(msg->piece_data);
                    msg->piece_data = NULL;
                    return -1;
                }
            }
            break;

        default:
            /*
             * Unknown message type. Per the spec, unknown messages should
             * be ignored. We consume the payload and continue.
             */
            fprintf(stderr, "peer %s:%d: unknown message id %d, skipping\n",
                    conn->ip, conn->port, id);
            if (payload_len > 0) {
                uint8_t *discard = xmalloc(payload_len);
                recv_all(conn->sock, discard, payload_len);
                free(discard);
            }
            break;
    }

    return 0;
}

/* ── peer_msg_free ───────────────────────────────────────────────────────── */

void peer_msg_free(PeerMsg *msg) {
    if (!msg) return;
    free(msg->bitfield);
    free(msg->piece_data);
    msg->bitfield   = NULL;
    msg->piece_data = NULL;
}

/* ── bitfield_has_piece ──────────────────────────────────────────────────── */

int bitfield_has_piece(const uint8_t *bitfield, int piece_index) {
    /*
     * Bitfield layout (MSB first):
     *
     *   Byte 0: [piece 0][piece 1][piece 2][piece 3][piece 4][piece 5][piece 6][piece 7]
     *   Byte 1: [piece 8][piece 9] ...
     *
     * To check piece i:
     *   - Byte index: i / 8
     *   - Bit within byte: bit 7 - (i % 8)    ← MSB = piece 0
     *
     * Example: piece 3 is in byte 0, bit 4
     *   (7 - 3 = 4, so mask = 0b00010000 = 0x10)
     */
    int byte_idx = piece_index / 8;
    int bit_idx  = 7 - (piece_index % 8);
    return (bitfield[byte_idx] >> bit_idx) & 1;
}
