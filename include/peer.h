/**
 * peer.h — Peer Wire Protocol
 *
 * Implements the BitTorrent peer wire protocol:
 *   - TCP connection establishment
 *   - Handshake (68-byte exchange to verify same torrent)
 *   - Message framing (4-byte length prefix + 1-byte message ID)
 *   - Sending: interested, request
 *   - Receiving: bitfield, have, unchoke, piece
 *
 * See docs/05_peer_protocol.md for full explanation.
 *
 * Usage:
 *   PeerConn *conn = peer_connect("1.2.3.4", 6881, info_hash, peer_id);
 *   if (!conn) { ... connection failed ... }
 *
 *   // Exchange handshake
 *   if (peer_handshake(conn) < 0) { peer_close(conn); return; }
 *
 *   // Send interested
 *   peer_send_interested(conn);
 *
 *   // Wait for unchoke, then request blocks
 *   PeerMsg msg;
 *   peer_recv_msg(conn, &msg);
 *   if (msg.type == MSG_UNCHOKE) {
 *       peer_send_request(conn, piece_idx, begin, length);
 *   }
 *   peer_msg_free(&msg);
 */

#ifndef BTORRENT_PEER_H
#define BTORRENT_PEER_H

#include <stdint.h>
#include <stddef.h>

/* Message type IDs */

/**
 * These are the single-byte message IDs used in the peer wire protocol.
 * -1 is our sentinel for "keep-alive" (length=0, no ID byte).
 */
typedef enum {
    MSG_KEEPALIVE    = -1,  /* length=0 message, no ID */
    MSG_CHOKE        =  0,  /* I won't send you data */
    MSG_UNCHOKE      =  1,  /* I will send you data */
    MSG_INTERESTED   =  2,  /* I want data from you */
    MSG_UNINTERESTED =  3,  /* I don't want data from you */
    MSG_HAVE         =  4,  /* I have piece [index] */
    MSG_BITFIELD     =  5,  /* Here's my bitfield of available pieces */
    MSG_REQUEST      =  6,  /* Please send me block [index, begin, length] */
    MSG_PIECE        =  7,  /* Here's block data [index, begin, data] */
    MSG_CANCEL       =  8,  /* Cancel request [index, begin, length] */
} MsgType;

/* Received message */

/**
 * PeerMsg - a parsed incoming message from a peer.
 *
 * After calling peer_recv_msg(), inspect msg.type and the appropriate fields.
 * Call peer_msg_free() when done (frees bitfield/piece data).
 */
typedef struct {
    MsgType  type;

    /* MSG_HAVE */
    uint32_t have_index;        /* which piece they finished */

    /* MSG_BITFIELD */
    uint8_t *bitfield;          /* heap-allocated bitfield bytes */
    uint32_t bitfield_len;      /* length in bytes */

    /* MSG_PIECE */
    uint32_t piece_index;       /* which piece */
    uint32_t piece_begin;       /* byte offset within piece */
    uint8_t *piece_data;        /* heap-allocated block data */
    uint32_t piece_data_len;    /* length of block data */

} PeerMsg;

/* Peer connection */

/**
 * PeerConn - an active TCP connection to a peer.
 */
typedef struct {
    int      sock;          /* TCP socket fd */
    char     ip[16];        /* peer's IP address */
    uint16_t port;          /* peer's port */
    uint8_t  peer_id[20];  /* their peer ID (filled in during handshake) */
    int      am_choked;     /* 1 = we are choked (can't request), 0 = unchoked */
    int      am_interested; /* 1 = we've sent interested message */
} PeerConn;

/* Connection lifecycle */

/**
 * peer_connect - open a TCP connection to a peer.
 *
 * @ip         IPv4 address string, e.g. "1.2.3.4"
 * @port       TCP port
 * @info_hash  20-byte info hash (stored for handshake)
 * @peer_id    our 20-byte peer ID (stored for handshake)
 *
 * @return heap-allocated PeerConn, or NULL on connection failure
 */
PeerConn *peer_connect(const char    *ip,
                       uint16_t       port,
                       const uint8_t *info_hash,
                       const uint8_t *peer_id);

/**
 * peer_close - close the connection and free the PeerConn.
 */
void peer_close(PeerConn *conn);

/* Handshake */

/**
 * peer_handshake - send our handshake, receive and verify theirs.
 *
 * The 68-byte handshake:
 *   [19]["BitTorrent protocol"][8 zero bytes][info_hash][peer_id]
 *
 * We verify that:
 *   - Their pstr is "BitTorrent protocol"
 *   - Their info_hash matches ours
 *
 * @return 0 on success, -1 on failure
 */
int peer_handshake(PeerConn *conn, const uint8_t *info_hash,
                   const uint8_t *our_peer_id);

/* Sending messages */

/** peer_send_interested - send MSG_INTERESTED (4-byte length + 1-byte ID) */
int peer_send_interested(PeerConn *conn);

/**
 * peer_send_request - send MSG_REQUEST to ask for a block.
 *
 * @conn       the peer connection
 * @index      piece index
 * @begin      byte offset within the piece
 * @length     block size (usually 16384 = 16 KiB)
 */
int peer_send_request(PeerConn *conn, uint32_t index,
                      uint32_t begin, uint32_t length);

/* Receiving messages */

/**
 * peer_recv_msg - receive and parse one message from the peer.
 *
 * Reads the 4-byte length prefix, then the message body.
 * Fills in the PeerMsg struct.
 *
 * @conn  the peer connection
 * @msg   output: parsed message (caller must call peer_msg_free when done)
 *
 * @return 0 on success, -1 on socket error or connection closed
 */
int peer_recv_msg(PeerConn *conn, PeerMsg *msg);

/**
 * peer_msg_free - free heap-allocated fields in a PeerMsg.
 * (Frees msg->bitfield and msg->piece_data if set.)
 */
void peer_msg_free(PeerMsg *msg);

/* Bitfield helpers */

/**
 * bitfield_has_piece - check if a bitfield indicates the peer has piece i.
 *
 * Bitfield format: most-significant bit of byte 0 = piece 0.
 * Byte 0 bit 7 = piece 0, byte 0 bit 6 = piece 1, ... byte 1 bit 7 = piece 8.
 */
int bitfield_has_piece(const uint8_t *bitfield, int piece_index);

#endif /* BTORRENT_PEER_H */
