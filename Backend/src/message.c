#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h> //Indicating Error 
#include <unistd.h> //Access Provider
#include <netdb.h> //Network Database
#include <arpa/inet.h> //Human redable ip address to binary
#include <netinet/in.h> //Ip address + port structure
#include <sys/socket.h> //Manage the process
#include <sys/types.h> //Os related info
#include <pthread.h> //Connection Estamblishing
#include "message.h"

uint64_t get_random_id(void) {
     uint64_t id;
     ssize_t ret;
     do {
        ret = getrandom(&id, sizeof(id), 0);
       } while (ret < 0 && errno == EINTR);

     if (ret < 0){
        return 0;
      }
    return id;
}

struct Message create_message(const char *message,struct User sender,struct User receiver) {
    struct Message msg;
    msg.message_id = get_random_id();
    msg.timestamp = time(NULL);
    snprintf(msg.message, sizeof(msg.message), "%s", message);
    msg.length = strlen(message);
    msg.sender = sender;
    msg.receiver = receiver;
    msg.status = STATUS_QUEUED;
    return msg;
};

int validate_message(struct Message msg){
    if (
      msg.message_id == 0 ||
      msg.length != strlen(msg.message) ||
      msg.message[0] == '\0' ||
      msg.timestamp == 0 ||
      msg.receiver.ip[0] == '\0'||
      msg.receiver.port <= 0 ||
      msg.receiver.port > 65535 ||
      msg.receiver.name[0] == '\0'||
      msg.sender.ip[0] == '\0' ||
      msg.sender.port <= 0 ||
      msg.sender.port > 65535 ||
      msg.sender.name[0] == '\0'
      ) {
          return 0;
       }
  return 1;
}

int send_message(int sockfd, struct Message *msg){
     if (msg == NULL) {
        return -1;
      }
     if (validate_message(*msg) == 0) {
       return -1;
     }
     if (sockfd < 0) {
       return -1;
     }
   size_t total = sizeof(*msg);
   size_t sent = 0;
     while (sent < total) {
         ssize_t n = send(sockfd ,(char *)msg + sent, total - sent, 0);
         if (n <= 0){
          return -1;
      }
        sent = sent + n;
     }
     msg->status = STATUS_SENT;
    return 0;
}

int receive_message

int main() {
 
