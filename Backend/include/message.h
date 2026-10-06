#include <time.h>
#include <stdint.h>
#include <stddef.h>
#ifndef MESSAGE_H
#define MESSAGE_H

struct User {
     char name[50];
     char ip[128];
     int port;
};

typedef enum {
  STATUS_SENT,
  STATUS_QUEUED,
  STATUS_DELIVERED,
  STATUS_FAILED
}Message_status;

struct Message {
     uint64_t message_id;
     time_t timestamp;
     char message[2048];
     size_t length;
     struct User sender;
     struct User receiver;
     Message_status status;
};
uint64_t get_random_id(void);

struct Message create_message(
    const char *message,
    struct User sender,
    struct User receiver
);

int validate_message(struct Message msg);
int send_message(int sockfd, struct Message *msg);
int receive_message(int sockfd, struct Message *msg);
int send_ack(int sockfd , uint64_t message_id);
int receive_ack(int sockfd , uint64_t *message_id);
int update_status(struct Message *msg, Message_status new_status);

#endif
