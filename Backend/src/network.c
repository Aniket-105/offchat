#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include "network.h"

int create_server(int port) {

    int server_fd;
    int opt = 1;
    struct sockaddr_in address;

    server_fd = socket(AF_INET , SOCK_STREAM , 0);
    if (server_fd < 0) {
       perror("Socket creation failed");
       return -1;
     }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))){
       perror("setsocket failed");
       close(server_fd);
       return -1;
     }

      memset(&address, 0, sizeof(address));
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      address.sin_addr.s_addr = INADDR_ANY;

     if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0){
       perror("Bind failed");
       close(server_fd);
       return -1;
     }

     if (listen(server_fd, 10) < 0){
        perror("Listen failed");
        close(server_fd);
        return -1;
     }
   return server_fd;
  }

int accept_client(int server_fd) {

       struct sockaddr_in client_addr;
       socklen_t client_len = sizeof(client_addr);

       int client_fd = accept( server_fd, (struct sockaddr *)&client_addr, &client_len);

       if ( client_fd < 0 ) {
          perror("Failed");
          return -1;
        }

       return client_fd;
}

int connect_to_server(const char *ip, int port)
{
    if (ip == NULL || port <= 0 || port > 65535) {
        errno = EINVAL;
        perror("Invalid IP or port");
        return -1;
    }

    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)port);

    int result = inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (result != 1) {
        if (result == 0) {
            fprintf(stderr, "Invalid IPv4 address: %s\n", ip);
        } else {
            perror("inet_pton failed");
        }

        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int close_socket(int sockfd)
{
    if (sockfd < 0) {
        errno = EINVAL;
        return -1;
    }

    return close(sockfd);
}
