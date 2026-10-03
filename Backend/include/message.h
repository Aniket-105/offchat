#include <time.h>
#include <stdint.h>
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
     int length;
     struct User sender;
     struct User receiver;
     Message_status status;
};

#endif
