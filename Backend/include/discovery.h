#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <netinet/in.h>

#define DISCOVERY_PORT 37020
#define PEER_NAME_SIZE 64
#define PEER_IP_SIZE 16

struct Peer {
    char name[PEER_NAME_SIZE];
    char ip[PEER_IP_SIZE];
    int port;
};

int create_discovery_socket(void);

int announce_peer(
    int udp_fd,
    const char *name,
    int tcp_port
);

int receive_peer(
    int udp_fd,
    struct Peer *peer,
    int timeout_ms
);

#endif
