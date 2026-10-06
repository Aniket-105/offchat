#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "network.h"

int create_server(int port) {
    int serverfd = socket(AF_INET6 , SOCK_STREAM , 0);
    if (server_fd < 0) {
       perror("Socket creation failed");
       return -1;
     }
    if (setsockopt(server_fd , SOL_SOCKET , SO_REUSEADDR , &opt , sizeof(opt))){
       perror("setsocket failed");
       close(server_fd);
       exit(EXIT_FAILURE);
     }
