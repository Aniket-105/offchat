#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define PROTOCOL_MAGIC 0x4F464348U
#define PROTOCOL_VERSION 1
#define PROTOCOL_MAX_PAYLOAD 65536

enum PacketType {
   PACKET_MESSAGE = 1,
   PACKET_REQUEST = 2,
   PACKET_ACCEPT = 3,
   PACKET_REJECT = 4
};

typedef enum {
    PACKET_MESSAGE = 1,
    PACKET_ACK     = 2,
    PACKET_FILE    = 3,
    PACKET_ERROR   = 4
} Packet_type;

struct PacketHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_length;
};

/* Send and receive exactly the requested number of bytes. */
int send_all(int sockfd, const void *buffer, size_t length);
int receive_all(int sockfd, void *buffer, size_t length);

/* Transfer a protocol header. */
int send_packet_header(int sockfd,
                       const struct PacketHeader *header);

int receive_packet_header(int sockfd,
                          struct PacketHeader *header);

#endif
