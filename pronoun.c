/*
* pronoun.c
* simple pronoun daemon client
* sends a request to the pronound daemon and receives the pronouns for a user
*
* pronound is free software distributed under GPLv3
*/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <username|uid>@<hostname>[:<port>]\n", argv[0]);
    }

    char *username_or_uid = strtok(argv[1], "@");
    char *hostname = strtok(NULL, " ");
    char *port_str = argv[2] ? argv[2] : "731";

    if (!username_or_uid) {
        fprintf(stderr, "Username or UID is required\n");
        return 1;
    }
    if (!hostname) {
        fprintf(stderr, "Hostname is required\n");
        return 1;
    } 

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP socket

    if (getaddrinfo(hostname, port_str, &hints, &res) != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(errno));
        return 1;
    }

    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (sockfd < 0) {
        fprintf(stderr, "socket creation failed: %s\n", strerror(errno));
        freeaddrinfo(res);
        return 1;
    }

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "connect failed: %s\n", strerror(errno));
        close(sockfd);
        freeaddrinfo(res);
        return 1;
    }

    freeaddrinfo(res);

    char request[256];
    snprintf(request, sizeof(request), "%s\n", username_or_uid);
    if (send(sockfd, request, strlen(request), 0) < 0) {
        fprintf(stderr, "send failed: %s\n", strerror(errno));
        close(sockfd);
        return 1;
    }

    char response[256];
    ssize_t bytes_received = recv(sockfd, response, sizeof(response) - 1, 0);
    if (bytes_received < 0) {
        fprintf(stderr, "recv failed: %s\n", strerror(errno));
        close(sockfd);
        return 1;
    }
    response[bytes_received] = '\0';
    printf("%s", response);
    close(sockfd);
    return 0;
}
