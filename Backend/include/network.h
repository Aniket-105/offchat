#ifndef NETWORK_H
#define NETWORK_H

int create_server(int port);
int accept_client(int server_fd);
int connect_to_server(const char *ip, int port);
int close_socket(int sockfd);

#endif
