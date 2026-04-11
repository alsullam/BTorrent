# BTorrent
<img src="./assets/images/BTlow.png" align="left" width="128" hspace="10" vspace="10">
<b>A BitTorrent Client Built From Scratch in C</b>.
<br/>A fully documented, educational BitTorrent client designed to teach you how peer-to-peer file sharing works at the lowest level.<br/><br/>

**Links:** [Alsullam](https://github.com/alsullam/) · [Doc](https://alsullam.github.io/projects/btorrent/)

BTorrent is a from-scratch BitTorrent client written in C, built to expose the inner workings of peer-to-peer systems at a low level. Instead of relying on high-level libraries, the project implements the protocol manually — from parsing `.torrent` files and handling bencoded data, to managing concurrent TCP connections and exchanging pieces across a swarm.

The goal is not just to download files, but to understand how decentralized networks coordinate, verify data integrity using SHA-1, and distribute load across peers efficiently. Each component is intentionally simple, modular, and well-documented.

---

## 🎯 What You'll Learn

By reading and running this code, you will understand:

1. **Bencoding** - BitTorrent's custom serialization format
2. **Torrent files** - How `.torrent` metadata is structured
3. **SHA-1 Hashing** - How file integrity is verified piece by piece
4. **Tracker Protocol** - HTTP and UDP tracker communication (BEP 3, BEP 15)
5. **Peer Protocol** - The TCP handshake, message wire format, and pipelining
6. **Piece Management** - How a file is split, downloaded, verified, and reassembled
7. **Concurrent Networking** - Non-blocking TCP with epoll and a per-peer state machine
8. **Structured Logging** - A leveled log system (`DEBUG` / `INFO` / `WARN` / `ERROR`)

---

## 📁 Project Structure

```
btorrent/
├── Makefile
├── include/
│   ├── cmd/
│   │   └── cmd.h              ← Shared Config struct and mode entry points
│   ├── core/
│   │   ├── bencode.h          ← Bencoding parser API
│   │   ├── pieces.h           ← Piece manager
│   │   ├── sha1.h             ← SHA-1 hash implementation
│   │   └── torrent.h          ← Torrent metadata structures
│   ├── net/
│   │   └── tcp.h              ← Non-blocking TCP helpers
│   ├── proto/
│   │   ├── peer.h             ← Peer wire protocol
│   │   └── tracker.h          ← HTTP + UDP tracker communication
│   ├── log.h                  ← Structured logging
│   ├── result.h               ← Unified error type (BtErr)
│   ├── scheduler.h            ← epoll-based concurrent download scheduler
│   └── utils.h                ← Utility helpers
├── src/
│   ├── cmd/
│   │   ├── cmd_check.c        ← -c: verify an existing download
│   │   ├── cmd_download.c     ← -d: full download lifecycle
│   │   └── cmd_inspect.c      ← -i: print torrent metadata
│   ├── core/
│   │   ├── bencode.c          ← Bencoding parser
│   │   ├── pieces.c           ← Piece download manager
│   │   ├── sha1.c             ← SHA-1 (RFC 3174)
│   │   └── torrent.c          ← .torrent file parser
│   ├── net/
│   │   └── tcp.c              ← Non-blocking connect / epoll helpers
│   ├── proto/
│   │   ├── peer.c             ← Peer handshake & messaging
│   │   └── tracker.c          ← HTTP + UDP tracker requests
│   ├── log.c                  ← Logging implementation
│   ├── main.c                 ← Argument parsing + mode dispatch
│   ├── result.c               ← bt_strerror
│   ├── scheduler.c            ← Concurrent epoll peer scheduler
│   └── utils.c                ← Utilities (random bytes, URL encode, etc.)
├── docs/
│   ├── 01_bencoding.md
│   ├── 02_torrent_file.md
│   ├── 03_sha1.md
│   ├── 04_tracker.md
│   ├── 05_peer_protocol.md
│   └── 06_pieces.md
└── tests/
    └── unit/
        ├── test_peer.c
        ├── test_pieces.c
        └── test_sha1.c
```

---

## 🛠 Building

Requires `gcc` and `libcurl`.

```bash
# Install dependency (Debian/Ubuntu)
sudo apt install libcurl4-openssl-dev

make          # Release build  → build/btorrent
make debug    # Debug build with AddressSanitizer + UBSan
make test     # Run unit tests
make clean    # Remove build artifacts
```

---

## 🚀 Usage

```
btorrent 0.2.0 — BitTorrent client

Usage:
  btorrent -d <file.torrent> [options]   Download a torrent
  btorrent -i <file.torrent> [-j]        Inspect torrent metadata
  btorrent -c <file.torrent> [-o path]   Check an existing download
  btorrent -V                            Print version and exit

Mode flags (pick one):
  -d, --download    Download the torrent
  -i, --inspect     Print metadata (no download)
  -c, --check       Verify an existing download against piece hashes
  -V, --version     Print version

Options:
  -o, --output <path>   Save directory            (default: current directory)
  -p, --port   <N>      Listen port               (default: 6881)
  -n, --peers  <N>      Max concurrent peers      (default: 50)
  -t, --timeout <N>     Per-peer connect timeout  (default: 5s)
  -P, --pipeline <N>    Block pipeline depth      (default: 64)
  -v, --verbose         Enable debug logging
  -l, --log <file>      Write log to file
  -j, --json            JSON output (inspect mode)
  -h, --help            Show this help
```

### Examples

```bash
# Download a torrent (saves to ./torrent-name/)
btorrent -d ubuntu.torrent

# Download to a specific directory
btorrent -d ubuntu.torrent -o ~/Downloads

# Inspect metadata
btorrent -i ubuntu.torrent

# Inspect and pipe to jq
btorrent -i ubuntu.torrent -j | jq .name

# Verify an existing download
btorrent -c ubuntu.torrent -o ~/Downloads

# Download with verbose logging written to a file
btorrent -d ubuntu.torrent -v -l btorrent.log
```

The `-o` flag sets the **download directory**. Files are saved inside it using the torrent's internal name, matching the behaviour of qBittorrent and Transmission.

Download can be interrupted with `Ctrl+C` at any time and resumed by running the same command again — the piece manager automatically detects and skips verified pieces on disk.

---

## 📖 Recommended Reading Order

Start with the docs, then read the source alongside them:

1. `docs/01_bencoding.md`     → `src/core/bencode.c`
2. `docs/02_torrent_file.md`  → `src/core/torrent.c`
3. `docs/03_sha1.md`          → `src/core/sha1.c`
4. `docs/04_tracker.md`       → `src/proto/tracker.c`
5. `docs/05_peer_protocol.md` → `src/proto/peer.c`
6. `docs/06_pieces.md`        → `src/core/pieces.c`
7. *(new)* `src/scheduler.c`  → epoll-based concurrent download engine
8. *(new)* `src/cmd/`         → how the three modes are orchestrated

---

## 🔗 BitTorrent Specification References

- [BEP 0003](https://www.bittorrent.org/beps/bep_0003.html) — The BitTorrent Protocol Specification
- [BEP 0015](https://www.bittorrent.org/beps/bep_0015.html) — UDP Tracker Protocol
- [BEP 0020](https://www.bittorrent.org/beps/bep_0020.html) — Peer ID Conventions
- [Bencoding](https://en.wikipedia.org/wiki/Bencode)
- [RFC 3174](https://tools.ietf.org/html/rfc3174) — SHA-1

---

## ⚠️ Educational Purpose

This client is built for learning. It deliberately implements the core protocol without:

- DHT (Distributed Hash Table)
- PEX (Peer Exchange)
- uTP (Micro Transport Protocol)
- Magnet links

These are excellent next steps once you understand the fundamentals here.
