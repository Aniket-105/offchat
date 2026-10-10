#include "discovery.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

int create_discovery_socket(void)
{
    int fd;
    int opt = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        perror("Discovery socket failed");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
                   &opt, sizeof(opt)) < 0) {
        perror("Discovery setsockopt failed");
        close(fd);
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(DISCOVERY_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&address,
             sizeof(address)) < 0) {
        perror("Discovery bind failed");
        close(fd);
        return -1;
    }

    return fd;
}

int announce_peer(int udp_fd, const char *name, int tcp_port)
{
    if (udp_fd < 0 || name == NULL ||
        name[0] == '\0' ||
        tcp_port < 1 || tcp_port > 65535) {
        errno = EINVAL;
        return -1;
    }

    /* Keep the simple text protocol unambiguous. */
    if (strchr(name, '|') != NULL ||
        strchr(name, '\n') != NULL) {
        errno = EINVAL;
        return -1;
    }

    char packet[128];

    int len = snprintf(packet, sizeof(packet),
                       "OFFCHAT/1|%d|%s",
                       tcp_port, name);

    if (len < 0 || (size_t)len >= sizeof(packet))
        return -1;

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));

    destination.sin_family = AF_INET;
    destination.sin_port = htons(DISCOVERY_PORT);

    if (inet_pton(AF_INET, "255.255.255.255",
                  &destination.sin_addr) != 1) {
        return -1;
    }

    ssize_t sent = sendto(
        udp_fd,
        packet,
        (size_t)len,
        0,
        (struct sockaddr *)&destination,
        sizeof(destination)
    );

    if (sent != len) {
        if (sent >= 0)
            errno = EIO;
        perror("Peer announcement failed");
        return -1;
    }

    return 0;
}

int receive_peer(int udp_fd, struct Peer *peer, int timeout_ms)
{
    if (udp_fd < 0 || peer == NULL || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(udp_fd, &readfds);

    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    int ready = select(udp_fd + 1, &readfds,
                       NULL, NULL, &timeout);

    if (ready < 0) {
        if (errno == EINTR)
            return 0;
        perror("Discovery select failed");
        return -1;
    }

    if (ready == 0)
        return 0; /* Timeout: no peer found. */

    char packet[128];
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);

    ssize_t received = recvfrom(
        udp_fd,
        packet,
        sizeof(packet) - 1,
        0,
        (struct sockaddr *)&sender,
        &sender_len
    );

    if (received < 0) {
        if (errno == EINTR)
            return 0;
        perror("Discovery receive failed");
        return -1;
    }

    packet[received] = '\0';

    char *prefix = "OFFCHAT/1|";

    if (strncmp(packet, prefix, strlen(prefix)) != 0)
        return 0; /* Not an OffChat announcement. */

    char *port_text = packet + strlen(prefix);
    char *separator = strchr(port_text, '|');

    if (separator == NULL)
        return 0;

    *separator = '\0';
    char *name = separator + 1;

    if (name[0] == '\0' ||
        strlen(name) >= sizeof(peer->name))
        return 0;

    char *endptr;
    errno = 0;
    long port = strtol(port_text, &endptr, 10);

    if (errno != 0 || *port_text == '\0' ||
        *endptr != '\0' || port < 1 || port > 65535)
        return 0;

    if (inet_ntop(AF_INET, &sender.sin_addr,
                  peer->ip, sizeof(peer->ip)) == NULL)
        return -1;

    strcpy(peer->name, name);
    peer->port = (int)port;

    return 1; /* One peer discovered. */
}
