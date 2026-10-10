#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int send_all(int sockfd, const void *buffer, size_t length)
{
    if (sockfd < 0 || (buffer == NULL && length > 0))
        return -1;

    const unsigned char *ptr = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(sockfd, ptr + sent,
                         length - sent, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

int receive_all(int sockfd, void *buffer, size_t length)
{
    if (sockfd < 0 || (buffer == NULL && length > 0))
        return -1;

    unsigned char *ptr = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t n = recv(sockfd, ptr + received,
                         length - received, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1; /* Peer disconnected. */

        received += (size_t)n;
    }

    return 0;
}

int send_packet_header(int sockfd,
                       const struct PacketHeader *header)
{
    if (header == NULL ||
        header->magic != PROTOCOL_MAGIC ||
        header->version != PROTOCOL_VERSION ||
        header->payload_length > PROTOCOL_MAX_PAYLOAD)
        return -1;

    uint32_t magic = htonl(header->magic);
    uint16_t version = htons(header->version);
    uint16_t type = htons(header->type);
    uint32_t payload_length = htonl(header->payload_length);

    /* Serialize fields individually: no struct padding on the wire. */
    if (send_all(sockfd, &magic, sizeof(magic)) < 0 ||
        send_all(sockfd, &version, sizeof(version)) < 0 ||
        send_all(sockfd, &type, sizeof(type)) < 0 ||
        send_all(sockfd, &payload_length,
                 sizeof(payload_length)) < 0)
        return -1;

    return 0;
}

int receive_packet_header(int sockfd,
                          struct PacketHeader *header)
{
    if (sockfd < 0 || header == NULL)
        return -1;

    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_length;

    if (receive_all(sockfd, &magic, sizeof(magic)) < 0 ||
        receive_all(sockfd, &version, sizeof(version)) < 0 ||
        receive_all(sockfd, &type, sizeof(type)) < 0 ||
        receive_all(sockfd, &payload_length,
                    sizeof(payload_length)) < 0)
        return -1;

    header->magic = ntohl(magic);
    header->version = ntohs(version);
    header->type = ntohs(type);
    header->payload_length = ntohl(payload_length);

    if (header->magic != PROTOCOL_MAGIC ||
        header->version != PROTOCOL_VERSION ||
        header->type < PACKET_MESSAGE ||
        header->type > PACKET_ERROR ||
        header->payload_length > PROTOCOL_MAX_PAYLOAD)
        return -1;

    return 0;
}
