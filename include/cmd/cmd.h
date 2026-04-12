#pragma once
/**
 * cmd.h — Shared Config struct and per-mode entry points
 *
 * Every mode (-d, -i, -c) receives the same parsed Config.
 * Return value is the process exit code (0 = success).
 */

#include <stdint.h>
#include <stdio.h>

/* ── Runtime config (populated by parse_args in main.c) ─────────────────── */

typedef enum {
    MODE_NONE     = 0,
    MODE_DOWNLOAD = 1,   /* -d / --download  */
    MODE_INSPECT  = 2,   /* -i / --inspect   */
    MODE_CHECK    = 3,   /* -c / --check     */
    MODE_VERSION  = 4,   /* -V / --version   */
} RunMode;

typedef struct {
    RunMode  mode;

    /* torrent + output */
    char     torrent_path[1024];
    char     output_path[1024];

    /* network */
    uint16_t port;
    int      max_peers;
    int      pipeline_depth;
    int      peer_timeout_s;

    /* logging */
    int      verbose;        /* -v  → LOG_DEBUG  */
    char     log_path[256];  /* -l  → write log to file */

    /* inspect-mode */
    int      json_output;    /* --json  → emit JSON instead of human text */

    /* magnet link support */
    int      is_magnet;      /* 1 if torrent_path is a magnet: URI */
} Config;

/* ── Per-mode entry points ───────────────────────────────────────────────── */

int cmd_download(const Config *cfg);
int cmd_inspect (const Config *cfg);
int cmd_check   (const Config *cfg);
