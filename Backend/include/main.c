
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "discovery.h"
#include "message.h"
#include "network.h"

#define OC_CAT_(a, b) a##b
#define OC_CAT(a, b)  OC_CAT_(a, b)
#define PACKET_MESSAGE OC_CAT(OC_PACKET_MESSAGE_, __COUNTER__)
#include "protocol.h"
#undef PACKET_MESSAGE
#define PACKET_MESSAGE 1
#include "protocol.c"

#define ANNOUNCE_INTERVAL_MS 2000
#define IO_TIMEOUT_SEC       5
#define MAX_WORKERS          32
#define MAX_LINE             8192
#define MAX_TEXT             (sizeof(((struct Message *)0)->message) - 1)

static struct User g_self;               
static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cnt_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_workers;

static void put_escaped(const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout);  break;
        case '\t': fputs("\\t", stdout);  break;
        case '\r': fputs("\\r", stdout);  break;
        default:   fputc(*s, stdout);     break;
        }
    }
}
static void emit(const char *event, ...)
{
    va_list ap;
    const char *field;

    pthread_mutex_lock(&g_out_lock);
    fputs(event, stdout);
    va_start(ap, event);
    while ((field = va_arg(ap, const char *)) != NULL) {
        fputc('\t', stdout);
        put_escaped(field);
    }
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_out_lock);
}

static int unescape(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r != '\\') {
            *w++ = *r++;
            continue;
        }
        r++;
        switch (*r) {
        case 'n':  *w++ = '\n'; break;
        case 't':  *w++ = '\t'; break;
        case 'r':  *w++ = '\r'; break;
        case '\\': *w++ = '\\'; break;
        default:   return -1;   /* includes a trailing lone backslash */
        }
        r++;
    }
    *w = '\0';
    return 0;
}

static void id_to_hex(uint64_t id, char out[17])
{
    snprintf(out, 17, "%016llx", (unsigned long long)id);
}
static int worker_acquire(void)
{
    int ok = 0;

    pthread_mutex_lock(&g_cnt_lock);
    if (g_workers < MAX_WORKERS) {
        g_workers++;
        ok = 1;
    }
    pthread_mutex_unlock(&g_cnt_lock);
    return ok;
}

static void worker_release(void)
{
    pthread_mutex_lock(&g_cnt_lock);
    g_workers--;
    pthread_mutex_unlock(&g_cnt_lock);
}

static int spawn_detached(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

static void set_io_timeouts(int fd)
{
    struct timeval tv = { .tv_sec = IO_TIMEOUT_SEC, .tv_usec = 0 };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int send_typed_header(int fd, uint16_t type, uint32_t len)
{
    struct PacketHeader h = {
        .magic = PROTOCOL_MAGIC,
        .version = PROTOCOL_VERSION,
        .type = type,
        .payload_length = len
    };
    return send_packet_header(fd, &h);
}

static int expect_header(int fd, uint16_t type, uint32_t len)
{
    struct PacketHeader h;

    if (receive_packet_header(fd, &h) < 0)
        return -1;
    return (h.type == type && h.payload_length == len) ? 0 : -1;
}


struct SendJob {
    char tag[32];
    char ip[64];
    int  port;
    char name[sizeof(((struct User *)0)->name)];
    char text[sizeof(((struct Message *)0)->message)];
};

static void *send_worker(void *arg)
{
    struct SendJob *job = arg;
    char idhex[17] = "0000000000000000";
    int fd = connect_to_server(job->ip, job->port);

    if (fd < 0) {
        emit("STATUS", job->tag, idhex, "FAILED", "could not connect", NULL);
        goto out;
    }
    set_io_timeouts(fd);
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    struct User sender = g_self;
    struct User receiver;

    if (getsockname(fd, (struct sockaddr *)&local, &local_len) < 0 ||
        inet_ntop(AF_INET, &local.sin_addr,
                  sender.ip, sizeof(sender.ip)) == NULL) {
        emit("STATUS", job->tag, idhex, "FAILED", "no local address", NULL);
        goto close_out;
    }

    memset(&receiver, 0, sizeof(receiver));
    snprintf(receiver.name, sizeof(receiver.name), "%s", job->name);
    snprintf(receiver.ip, sizeof(receiver.ip), "%s", job->ip);
    receiver.port = job->port;

    struct Message msg = create_message(job->text, sender, receiver);
    id_to_hex(msg.message_id, idhex);

    if (send_typed_header(fd, PACKET_MESSAGE, sizeof(struct Message)) < 0 ||
        send_message(fd, &msg) < 0) {
        update_status(&msg, STATUS_FAILED);
        emit("STATUS", job->tag, idhex, "FAILED", "send failed", NULL);
        goto close_out;
    }
    emit("STATUS", job->tag, idhex, "SENT", "", NULL);

    uint64_t acked = 0;

    if (expect_header(fd, PACKET_ACK, sizeof(uint64_t)) < 0 ||
        receive_ack(fd, &acked) < 0) {
        update_status(&msg, STATUS_FAILED);
        emit("STATUS", job->tag, idhex, "FAILED", "no acknowledgement", NULL);
    } else if (acked != msg.message_id) {
        update_status(&msg, STATUS_FAILED);
        emit("STATUS", job->tag, idhex, "FAILED", "wrong acknowledgement", NULL);
    } else {
        update_status(&msg, STATUS_DELIVERED);
        emit("STATUS", job->tag, idhex, "DELIVERED", "", NULL);
    }

close_out:
    close_socket(fd);
out:
    free(job);
    worker_release();
    return NULL;
}

struct RecvJob {
    int  fd;
    char ip[INET_ADDRSTRLEN];
};

static void *recv_worker(void *arg)
{
    struct RecvJob *job = arg;
    struct Message msg;

    set_io_timeouts(job->fd);

    if (expect_header(job->fd, PACKET_MESSAGE, sizeof(struct Message)) < 0 ||
        receive_message(job->fd, &msg) < 0)
        goto out;

    if (send_typed_header(job->fd, PACKET_ACK, sizeof(uint64_t)) == 0)
        send_ack(job->fd, msg.message_id);

    char idhex[17], port[16];
    id_to_hex(msg.message_id, idhex);
    snprintf(port, sizeof(port), "%d", msg.sender.port);
    emit("MSG", idhex, msg.sender.name, job->ip, port, msg.message, NULL);

out:
    close_socket(job->fd);
    free(job);
    worker_release();
    return NULL;
}

static void handle_incoming(int server_fd)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd = accept_client(server_fd);

    if (fd < 0)
        return;

    struct RecvJob *job = calloc(1, sizeof(*job));

    if (job == NULL || !worker_acquire()) {
        free(job);
        close_socket(fd);
        return;
    }

    job->fd = fd;
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) < 0 ||
        inet_ntop(AF_INET, &peer.sin_addr, job->ip, sizeof(job->ip)) == NULL)
        snprintf(job->ip, sizeof(job->ip), "0.0.0.0");

    if (spawn_detached(recv_worker, job) != 0) {
        close_socket(fd);
        free(job);
        worker_release();
    }
}

static int is_local_ip(const char *ip)
{
    struct ifaddrs *list, *it;
    struct in_addr want;
    int found = 0;

    if (inet_pton(AF_INET, ip, &want) != 1)
        return 0;
    if (getifaddrs(&list) < 0)
        return 0;

    for (it = list; it != NULL && !found; it = it->ifa_next) {
        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET)
            continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)it->ifa_addr;
        found = (sin->sin_addr.s_addr == want.s_addr);
    }
    freeifaddrs(list);
    return found;
}

static void drain_discovery(int udp_fd)
{
    struct Peer p;

    while (receive_peer(udp_fd, &p, 0) > 0) {
        if (p.port == g_self.port && is_local_ip(p.ip))
            continue;                       /* that is us */

        char port[16];
        snprintf(port, sizeof(port), "%d", p.port);
        emit("PEER", p.name, p.ip, port, NULL);
    }
}

static int split_tabs(char *line, char **fields, int max)
{
    int n = 0;

    fields[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == '\t') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

static void fail_send(const char *tag, const char *why)
{
    emit("STATUS", tag, "0000000000000000", "FAILED", why, NULL);
}

static void cmd_send(char *line)
{
    char *f[6];
    char *end;

    /* SEND tag ip port name text */
    if (split_tabs(line, f, 6) != 6) {
        emit("WARN", "malformed SEND", NULL);
        return;
    }

    const char *tag = f[1];

    if (unescape(f[4]) < 0 || unescape(f[5]) < 0) {
        fail_send(tag, "bad escape sequence");
        return;
    }

    size_t len = strlen(f[5]);

    if (len == 0) {
        fail_send(tag, "empty message");
        return;
    }
    if (len > MAX_TEXT) {
        fail_send(tag, "message too long");
        return;
    }

    errno = 0;
    long port = strtol(f[3], &end, 10);

    if (errno != 0 || *f[3] == '\0' || *end != '\0' ||
        port < 1 || port > 65535) {
        fail_send(tag, "invalid port");
        return;
    }

    struct SendJob *job = calloc(1, sizeof(*job));

    if (job == NULL || !worker_acquire()) {
        free(job);
        fail_send(tag, "too many pending sends");
        return;
    }

    snprintf(job->tag, sizeof(job->tag), "%s", tag);
    snprintf(job->ip, sizeof(job->ip), "%s", f[2]);
    snprintf(job->name, sizeof(job->name), "%s", f[4][0] ? f[4] : "peer");
    snprintf(job->text, sizeof(job->text), "%s", f[5]);
    job->port = (int)port;

    if (spawn_detached(send_worker, job) != 0) {
        free(job);
        worker_release();
        fail_send(tag, "could not start sender");
    }
}

static int handle_line(char *line)
{
    size_t n = strlen(line);

    if (n > 0 && line[n - 1] == '\r')
        line[n - 1] = '\0';

    if (strcmp(line, "QUIT") == 0)
        return 0;
    if (strncmp(line, "SEND\t", 5) == 0)
        cmd_send(line);
    else if (line[0] != '\0')
        emit("WARN", "unknown command", NULL);
    return 1;
}

static int read_commands(void)
{
    static char buf[MAX_LINE];
    static size_t used;
    static int skipping;

    ssize_t n = read(STDIN_FILENO, buf + used, sizeof(buf) - used);

    if (n < 0)
        return (errno == EINTR || errno == EAGAIN) ? 1 : 0;
    if (n == 0)
        return 0;
    used += (size_t)n;

    size_t start = 0;

    for (size_t i = 0; i < used; i++) {
        if (buf[i] != '\n')
            continue;
        buf[i] = '\0';
        if (skipping)
            skipping = 0;           
        else if (!handle_line(buf + start))
            return 0;
        start = i + 1;
    }

    memmove(buf, buf + start, used - start);
    used -= start;

    if (used == sizeof(buf)) {     
        emit("WARN", "command line too long, dropped", NULL);
        used = 0;
        skipping = 1;
    }
    return 1;
}
static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int valid_name(const char *name)
{
    size_t len = strlen(name);

    if (len == 0 || len >= sizeof(g_self.name))
        return 0;
    for (const char *p = name; *p; p++)
        if (*p == '|' || *p == '\t' || *p == '\n' || *p == '\r')
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <name> <tcp_port>\n", argv[0]);
        return 2;
    }

    char *end;
    long port = strtol(argv[2], &end, 10);

    if (*argv[2] == '\0' || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[2]);
        return 2;
    }
    if (!valid_name(argv[1])) {
        fprintf(stderr, "invalid name (1-%zu bytes, no | or control chars)\n",
                sizeof(g_self.name) - 1);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);   

    memset(&g_self, 0, sizeof(g_self));
    snprintf(g_self.name, sizeof(g_self.name), "%s", argv[1]);
    g_self.port = (int)port;

    int server_fd = create_server(g_self.port);

    if (server_fd < 0) {
        emit("ERROR", "cannot listen on that port (already in use?)", NULL);
        return 1;
    }

    int udp_fd = create_discovery_socket();

    if (udp_fd < 0)
        emit("WARN", "peer discovery unavailable; add peers manually", NULL);

    char portstr[16];

    snprintf(portstr, sizeof(portstr), "%d", g_self.port);
    emit("READY", g_self.name, portstr, NULL);

    int announce_ok = 1;
    long long next_announce = now_ms();
    int running = 1;

    while (running) {
        long long now = now_ms();

        if (udp_fd >= 0 && now >= next_announce) {
            int ok = (announce_peer(udp_fd, g_self.name, g_self.port) == 0);

            if (ok != announce_ok) {
                announce_ok = ok;
                emit("WARN", ok ? "discovery broadcast working again"
                                : "discovery broadcast failing (no network?)",
                     NULL);
            }
            next_announce = now + ANNOUNCE_INTERVAL_MS;
        }

        int timeout = (udp_fd >= 0) ? (int)(next_announce - now_ms()) : 1000;

        if (timeout < 0)
            timeout = 0;

        struct pollfd fds[3] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = server_fd,    .events = POLLIN },
            { .fd = udp_fd,       .events = POLLIN }  /* -1 is ignored */
        };

        if (poll(fds, 3, timeout) < 0) {
            if (errno == EINTR)
                continue;
            emit("ERROR", "poll failed", NULL);
            break;
        }

        if (fds[2].revents & POLLIN)
            drain_discovery(udp_fd);
        if (fds[1].revents & POLLIN)
            handle_incoming(server_fd);
        if (fds[0].revents & (POLLIN | POLLHUP))
            running = read_commands();
    }
    return 0;
}
