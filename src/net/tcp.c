#define _POSIX_C_SOURCE 200809L
/**
 * net/tcp.c — Non-blocking TCP helpers
 */

#include "net/tcp.h"
#include "log.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int tcp_connect_nb(const char *ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    /* Non-blocking */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock); return -1;
    }

    /* TCP_NODELAY — small messages (REQUEST = 17 bytes) must not be batched */
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(sock); return -1;
    }

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(sock); return -1;
    }
    return sock;
}

int tcp_finish_connect(int sock) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return -1;
    if (err != 0) {
        LOG_DEBUG("tcp_finish_connect: %s", strerror(err));
        return -1;
    }
    return 0;
}

void tcp_set_timeouts(int sock, int seconds) {
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
