/**
 * aesdsocket.c - Socket server for AESD Assignment 5
 *
 * Opens a stream socket on port 9000, accepts connections,
 * appends received data to /var/tmp/aesdsocketdata (newline delimited),
 * and returns the full file content to the client after each packet.
 *
 * Supports -d flag for daemon mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT          "9000"
#define DATA_FILE     "/var/tmp/aesdsocketdata"
#define BACKLOG       10
#define RECV_BUF_SIZE 1024

/* Global file descriptor for the listening socket so signal handler can close it */
static int server_fd = -1;
/* Flag set by signal handler to request graceful shutdown */
static volatile sig_atomic_t caught_signal = 0;

/* ──────────────────────────── signal handling ─────────────────────────────── */

static void signal_handler(int signo)
{
    if(signo == SIGINT || signo == SIGTERM) {
        caught_signal = 1;
        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
    }

}

static int setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        return -1;
    }
    return 0;
}

/* ───────────────────────────── daemon helpers ─────────────────────────────── */

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid > 0) {
        /* Parent exits */
        exit(EXIT_SUCCESS);
    }

    /* Child: become session leader */
    if (setsid() == -1) {
        perror("setsid");
        return -1;
    }

    /* Redirect stdin/stdout/stderr to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull != -1) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }

    return 0;
}

/* ──────────────────────────── socket setup ────────────────────────────────── */

/**
 * Create, bind, and start listening on port PORT.
 * Returns the listening socket fd on success, -1 on failure.
 */
static int create_server_socket(void)
{
    struct addrinfo hints, *res, *rp;
    int fd = -1;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    int rc = getaddrinfo(NULL, PORT, &hints, &res);
    if (rc != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(rc));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1)
            continue;

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
            close(fd);
            fd = -1;
            continue;
        }

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break; /* success */

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd == -1) {
        syslog(LOG_ERR, "Failed to bind socket: %s", strerror(errno));
        return -1;
    }

    if (listen(fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ──────────────────────────── client helpers ──────────────────────────────── */

/**
 * Get a printable IP address string from a sockaddr (IPv4 or IPv6).
 * Writes into buf (INET6_ADDRSTRLEN bytes recommended).
 */
static void get_peer_ip(const struct sockaddr_storage *ss, char *buf, size_t buflen)
{
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &s->sin_addr, buf, (socklen_t)buflen);
    } else {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)ss;
        inet_ntop(AF_INET6, &s->sin6_addr, buf, (socklen_t)buflen);
    }
}

/**
 * Handle one accepted client connection:
 *   - Receive data until a newline, accumulating in a heap buffer.
 *   - Append to DATA_FILE.
 *   - Send the full DATA_FILE content back to the client.
 *
 * Returns 0 on success, -1 on fatal error.
 */
static int handle_client(int client_fd)
{
    char recv_buf[RECV_BUF_SIZE];
    char *packet_buf  = NULL;
    size_t packet_len = 0;
    ssize_t n;
    int ret = 0;

    /* ── Receive until newline ── */
    while (1) {
        n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "recv: %s", strerror(errno));
            ret = -1;
            goto cleanup_recv;
        }
        if (n == 0)
            break; /* client closed connection before newline */

        /* Grow packet buffer */
        char *new_buf = realloc(packet_buf, packet_len + (size_t)n + 1);
        if (!new_buf) {
            syslog(LOG_ERR, "realloc failed: %s", strerror(errno));
            ret = -1;
            goto cleanup_recv;
        }
        packet_buf = new_buf;
        memcpy(packet_buf + packet_len, recv_buf, (size_t)n);
        packet_len += (size_t)n;
        packet_buf[packet_len] = '\0';

        /* Check for newline - packet complete */
        if (memchr(packet_buf, '\n', packet_len) != NULL)
            break;
    }

    if (packet_len == 0)
        goto cleanup_recv;

    /* ── Append to DATA_FILE ── */
    int data_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (data_fd == -1) {
        syslog(LOG_ERR, "open %s: %s", DATA_FILE, strerror(errno));
        ret = -1;
        goto cleanup_recv;
    }

    size_t written = 0;
    while (written < packet_len) {
        ssize_t w = write(data_fd, packet_buf + written, packet_len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "write: %s", strerror(errno));
            ret = -1;
            close(data_fd);
            goto cleanup_recv;
        }
        written += (size_t)w;
    }
    close(data_fd);

    /* ── Send full DATA_FILE back ── */
    data_fd = open(DATA_FILE, O_RDONLY);
    if (data_fd == -1) {
        syslog(LOG_ERR, "open %s for read: %s", DATA_FILE, strerror(errno));
        ret = -1;
        goto cleanup_recv;
    }

    while (1) {
        ssize_t r = read(data_fd, recv_buf, sizeof(recv_buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "read: %s", strerror(errno));
            ret = -1;
            break;
        }
        if (r == 0)
            break;

        ssize_t sent = 0;
        while (sent < r) {
            ssize_t s = send(client_fd, recv_buf + sent, (size_t)(r - sent), 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "send: %s", strerror(errno));
                ret = -1;
                goto cleanup_read;
            }
            sent += s;
        }
    }

cleanup_read:
    close(data_fd);

cleanup_recv:
    free(packet_buf);
    return ret;
}

/* ──────────────────────────────── main ────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int daemon_mode = 0;

    /* Parse arguments */
    int opt;
    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
        case 'd':
            daemon_mode = 1;
            break;
        default:
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            return -1;
        }
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (setup_signals() != 0) {
        closelog();
        return -1;
    }

    /* Create & bind the socket before forking so we can return -1 on failure */
    server_fd = create_server_socket();
    if (server_fd == -1) {
        closelog();
        return -1;
    }

    /* Daemonize AFTER successful bind */
    if (daemon_mode) {
        if (daemonize() != 0) {
            close(server_fd);
            closelog();
            return -1;
        }
    }

    /* ── Accept loop ── */
    while (!caught_signal) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            if (errno == EINTR || caught_signal)
                break;
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            continue;
        }

        char client_ip[INET6_ADDRSTRLEN] = {0};
        get_peer_ip(&client_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        handle_client(client_fd);

        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        close(client_fd);
    }

    /* ── Graceful shutdown ── */
    syslog(LOG_INFO, "Caught signal, exiting");

    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }

    /* Delete the data file */
    if (remove(DATA_FILE) != 0 && errno != ENOENT) {
        syslog(LOG_ERR, "remove %s: %s", DATA_FILE, strerror(errno));
    }

    closelog();
    return 0;
}
