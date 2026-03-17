/**
 * aesdsocket.c - Socket server for AESD Assignment 6
 *
 * Opens a stream socket on port 9000, accepts connections,
 * spawns a new thread per connection, appends received data
 * to /var/tmp/aesdsocketdata (newline delimited), and returns
 * the full file content to the client after each packet.
 *
 * All writes to the data file are serialised with a mutex.
 *
 * A timer thread appends "timestamp:<RFC2822>\n" every 10 s.
 *
 * Threads are tracked in a singly-linked list and joined on
 * shutdown (no detached threads).
 *
 * Supports -d flag for daemon mode.
 *
 * Build switch:
 *   USE_AESD_CHAR_DEVICE=1 (default) - use /dev/aesdchar, no timestamps
 *   USE_AESD_CHAR_DEVICE=0           - use /var/tmp/aesdsocketdata, timestamps enabled
 */

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if USE_AESD_CHAR_DEVICE
#include "../aesd-char-driver/aesd_ioctl.h"
#endif

#define PORT               "9000"
#if USE_AESD_CHAR_DEVICE
#define DATA_FILE          "/dev/aesdchar"
#else
#define DATA_FILE          "/var/tmp/aesdsocketdata"
#endif
#define BACKLOG            10
#define RECV_BUF_SIZE      1024
#define TIMESTAMP_INTERVAL 10   /* seconds between timestamp writes */

/* ─── globals ──────────────────────────────────────────────────────────────── */

static int                    server_fd     = -1;
static volatile sig_atomic_t  caught_signal = 0;

/* Single mutex that serialises every access to DATA_FILE */
static pthread_mutex_t  file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ─── thread list (singly-linked via sys/queue.h SLIST) ──────────────────── */

struct thread_node {
    pthread_t        tid;
    int              client_fd;
    char             client_ip[INET6_ADDRSTRLEN];
    volatile int     done;   /* set to 1 by thread on exit */
    SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(thread_list, thread_node);

static struct thread_list  thread_list_head = SLIST_HEAD_INITIALIZER(thread_list_head);
static pthread_mutex_t     list_mutex       = PTHREAD_MUTEX_INITIALIZER;

static void list_add(struct thread_node *node)
{
    pthread_mutex_lock(&list_mutex);
    SLIST_INSERT_HEAD(&thread_list_head, node, entries);
    pthread_mutex_unlock(&list_mutex);
}

/* Join and free nodes whose threads have finished */
static void list_reap_done(void)
{
    pthread_mutex_lock(&list_mutex);
    struct thread_node *n = SLIST_FIRST(&thread_list_head);
    while (n != NULL) {
        struct thread_node *next = SLIST_NEXT(n, entries);
        if (n->done) {
            pthread_join(n->tid, NULL);
            SLIST_REMOVE(&thread_list_head, n, thread_node, entries);
            free(n);
        }
        n = next;
    }
    pthread_mutex_unlock(&list_mutex);
}

/* Shutdown all client sockets to unblock any blocked recv/send */
static void list_shutdown_all(void)
{
    pthread_mutex_lock(&list_mutex);
    struct thread_node *n;
    SLIST_FOREACH(n, &thread_list_head, entries) {
        if (!n->done && n->client_fd != -1)
            shutdown(n->client_fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&list_mutex);
}

/* Join every remaining thread (call after list_shutdown_all) */
static void list_join_all(void)
{
    pthread_mutex_lock(&list_mutex);
    struct thread_node *n;
    while (!SLIST_EMPTY(&thread_list_head)) {
        n = SLIST_FIRST(&thread_list_head);
        SLIST_REMOVE_HEAD(&thread_list_head, entries);
        pthread_mutex_unlock(&list_mutex);
        pthread_join(n->tid, NULL);
        free(n);
        pthread_mutex_lock(&list_mutex);
    }
    pthread_mutex_unlock(&list_mutex);
}

/* ─── signal handling ─────────────────────────────────────────────────────── */

static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
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

    if (sigaction(SIGINT,  &sa, NULL) == -1) { perror("sigaction SIGINT");  return -1; }
    if (sigaction(SIGTERM, &sa, NULL) == -1) { perror("sigaction SIGTERM"); return -1; }
    return 0;
}

/* ───────────────────────────── daemon helpers ─────────────────────────────── */

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0)  { perror("fork"); return -1; }
    if (pid > 0)  exit(EXIT_SUCCESS);

    if (setsid() == -1) { perror("setsid"); return -1; }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull != -1) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
    return 0;
}

/* ─── socket setup ────────────────────────────────────────────────────────── */

static int create_server_socket(void)
{
    struct addrinfo hints, *res, *rp;
    int fd = -1, yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    int rc = getaddrinfo(NULL, PORT, &hints, &res);
    if (rc != 0) { syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(rc)); return -1; }

    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
            close(fd); fd = -1; continue;
        }
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);

    if (fd == -1) { syslog(LOG_ERR, "Failed to bind: %s", strerror(errno)); return -1; }
    if (listen(fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

/* ─── helper: extract peer IP ─────────────────────────────────────────────── */

static void get_peer_ip(const struct sockaddr_storage *ss, char *buf, size_t len)
{
    if (ss->ss_family == AF_INET)
        inet_ntop(AF_INET,  &((const struct sockaddr_in  *)ss)->sin_addr,  buf, (socklen_t)len);
    else
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)ss)->sin6_addr, buf, (socklen_t)len);
}

/* ─── per-connection thread ───────────────────────────────────────────────── */

static void *client_thread(void *arg)
{
    struct thread_node *node = (struct thread_node *)arg;
    int client_fd = node->client_fd;

    char   recv_buf[RECV_BUF_SIZE];
    char  *packet_buf = NULL;
    size_t packet_len = 0;
    int    ret        = 0;

    /* ── Receive data until newline ── */
    while (1) {
        ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "recv: %s", strerror(errno));
            ret = -1;
            goto cleanup_recv;
        }
        if (n == 0) break;  /* client closed connection */

        char *nb = realloc(packet_buf, packet_len + (size_t)n + 1);
        if (!nb) {
            syslog(LOG_ERR, "realloc: %s", strerror(errno));
            ret = -1;
            goto cleanup_recv;
        }
        packet_buf = nb;
        memcpy(packet_buf + packet_len, recv_buf, (size_t)n);
        packet_len += (size_t)n;
        packet_buf[packet_len] = '\0';

        if (memchr(packet_buf, '\n', packet_len)) break;
    }

    if (packet_len == 0) goto cleanup_recv;

#if USE_AESD_CHAR_DEVICE
    /* Handle AESDCHAR_IOCSEEKTO ioctl command */
    if (strncmp(packet_buf, "AESDCHAR_IOCSEEKTO:", 19) == 0) {
        unsigned int write_cmd, write_cmd_offset;
        struct aesd_seekto seekto;
        int data_fd;

        if (sscanf(packet_buf + 19, "%u,%u", &write_cmd, &write_cmd_offset) != 2) {
            syslog(LOG_ERR, "Invalid AESDCHAR_IOCSEEKTO format");
            goto cleanup_recv;
        }

        seekto.write_cmd = write_cmd;
        seekto.write_cmd_offset = write_cmd_offset;

        pthread_mutex_lock(&file_mutex);

        data_fd = open(DATA_FILE, O_RDWR);
        if (data_fd == -1) {
            syslog(LOG_ERR, "open %s: %s", DATA_FILE, strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            goto cleanup_recv;
        }

        if (ioctl(data_fd, AESDCHAR_IOCSEEKTO, &seekto) != 0) {
            syslog(LOG_ERR, "ioctl AESDCHAR_IOCSEEKTO: %s", strerror(errno));
            close(data_fd);
            pthread_mutex_unlock(&file_mutex);
            goto cleanup_recv;
        }

        /* Read from the seeked position and send to client */
        while (1) {
            ssize_t r = read(data_fd, recv_buf, sizeof(recv_buf));
            if (r < 0) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "read: %s", strerror(errno));
                break;
            }
            if (r == 0) break;

            for (ssize_t sent = 0; sent < r; ) {
                ssize_t s = send(client_fd, recv_buf + sent, (size_t)(r - sent), 0);
                if (s < 0) {
                    if (errno == EINTR) continue;
                    syslog(LOG_ERR, "send: %s", strerror(errno));
                    goto ioctl_cleanup;
                }
                sent += s;
            }
        }

    ioctl_cleanup:
        close(data_fd);
        pthread_mutex_unlock(&file_mutex);
        goto cleanup_recv;
    }
#endif

    /* ── Lock mutex: append to file, then send full file ── */
    pthread_mutex_lock(&file_mutex);

    /* Append received packet */
#if USE_AESD_CHAR_DEVICE
    int data_fd = open(DATA_FILE, O_WRONLY);
#else
    int data_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
    if (data_fd == -1) {
        syslog(LOG_ERR, "open %s (write): %s", DATA_FILE, strerror(errno));
        ret = -1;
        pthread_mutex_unlock(&file_mutex);
        goto cleanup_recv;
    }

    for (size_t written = 0; written < packet_len; ) {
        ssize_t w = write(data_fd, packet_buf + written, packet_len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "write: %s", strerror(errno));
            ret = -1;
            close(data_fd);
            pthread_mutex_unlock(&file_mutex);
            goto cleanup_recv;
        }
        written += (size_t)w;
    }
    close(data_fd);

    /* Send full data file back to client */
    data_fd = open(DATA_FILE, O_RDONLY);
    if (data_fd == -1) {
        syslog(LOG_ERR, "open %s (read): %s", DATA_FILE, strerror(errno));
        ret = -1;
        pthread_mutex_unlock(&file_mutex);
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
        if (r == 0) break;

        for (ssize_t sent = 0; sent < r; ) {
            ssize_t s = send(client_fd, recv_buf + sent, (size_t)(r - sent), 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "send: %s", strerror(errno));
                ret = -1;
                goto cleanup_read_locked;
            }
            sent += s;
        }
    }

cleanup_read_locked:
    close(data_fd);
    pthread_mutex_unlock(&file_mutex);

cleanup_recv:
    free(packet_buf);
    (void)ret;

    syslog(LOG_INFO, "Closed connection from %s", node->client_ip);
    close(client_fd);
    node->done = 1;
    return NULL;
}

/* ─── timer thread (timestamp every TIMESTAMP_INTERVAL seconds) ───────────── */

#if !USE_AESD_CHAR_DEVICE
static void *timer_thread(void *arg)
{
    (void)arg;
    int elapsed = 0;

    while (!caught_signal) {
        sleep(1);
        if (caught_signal) break;
        if (++elapsed < TIMESTAMP_INTERVAL) continue;
        elapsed = 0;

        /* Build RFC 2822 timestamp line */
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        char ts[64];
        strftime(ts, sizeof(ts), "%a, %d %b %Y %T %z", &tm_info);

        char line[128];
        int  llen = snprintf(line, sizeof(line), "timestamp:%s\n", ts);
        if (llen < 0 || llen >= (int)sizeof(line)) {
            syslog(LOG_ERR, "timer: snprintf overflow");
            continue;
        }

        pthread_mutex_lock(&file_mutex);

        int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd == -1) {
            syslog(LOG_ERR, "timer: open %s: %s", DATA_FILE, strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            continue;
        }

        for (size_t written = 0; written < (size_t)llen; ) {
            ssize_t w = write(fd, line + written, (size_t)llen - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "timer: write: %s", strerror(errno));
                break;
            }
            written += (size_t)w;
        }

        close(fd);
        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}
#endif /* !USE_AESD_CHAR_DEVICE */

/* ─── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    int opt;

    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
        case 'd': daemon_mode = 1; break;
        default:
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            return -1;
        }
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (setup_signals() != 0) { closelog(); return -1; }

    server_fd = create_server_socket();
    if (server_fd == -1) { closelog(); return -1; }

    if (daemon_mode && daemonize() != 0) {
        close(server_fd); closelog(); return -1;
    }

    /* Start the timestamp timer thread (only when not using char device) */
#if !USE_AESD_CHAR_DEVICE
    pthread_t timer_tid;
    if (pthread_create(&timer_tid, NULL, timer_thread, NULL) != 0) {
        syslog(LOG_ERR, "pthread_create timer: %s", strerror(errno));
        close(server_fd); closelog(); return -1;
    }
#endif

    /* ── Accept loop ── */
    while (!caught_signal) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            if (errno == EINTR || caught_signal) break;
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            continue;
        }

        struct thread_node *node = calloc(1, sizeof(*node));
        if (!node) {
            syslog(LOG_ERR, "calloc: %s", strerror(errno));
            close(client_fd);
            continue;
        }

        node->client_fd = client_fd;
        get_peer_ip(&client_addr, node->client_ip, sizeof(node->client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", node->client_ip);

        if (pthread_create(&node->tid, NULL, client_thread, node) != 0) {
            syslog(LOG_ERR, "pthread_create: %s", strerror(errno));
            close(client_fd);
            free(node);
            continue;
        }

        list_add(node);

        /* Opportunistically clean up any finished threads */
        list_reap_done();
    }

    /* ── Graceful shutdown ── */
    syslog(LOG_INFO, "Caught signal, exiting");

    /* Unblock any threads blocked in recv/send */
    list_shutdown_all();

    /* Wait for all client threads */
    list_join_all();

#if !USE_AESD_CHAR_DEVICE
    /* Wait for timer thread (exits within 1 s via caught_signal check) */
    pthread_join(timer_tid, NULL);
#endif

    if (server_fd != -1) { close(server_fd); server_fd = -1; }

#if !USE_AESD_CHAR_DEVICE
    if (remove(DATA_FILE) != 0 && errno != ENOENT)
        syslog(LOG_ERR, "remove %s: %s", DATA_FILE, strerror(errno));
#endif

    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&list_mutex);

    closelog();
    return 0;
}
