#include<errno.h>
#include<unistd.h>
#include<sys/socket.h>
#include<netinet/ip.h>
#include<sys/epoll.h>
#include<stdio.h>
#include<string.h>

/*
 * This is a simple example of a scalable poll-based HTTP server.
 *
 * There are three kind of file descriptors that are relevant here:
 *  - one for epoll (via epoll_create)
 *  - one for the socket (via socket)
 *  - one for each individual TCP connections (via accept)
 *
 *  We monitor read events on the socket to accept new connections and
 *  read-write events on each individual connections.
 *
 *  Once a connection has been processed, it is immediately closed. We are not
 *  performing any HTTP pipelining here for sake of simplicity.
 */

static void
close_fd(int* fd) {
    if (*fd != -1) {
        printf("Closing fd %d...\n", *fd);
        close (*fd);
    }
}

int
main() {
    const int maxconnections = 10;

    int numconnections = 0;

    // structure containing the events being read
    struct epoll_event events[1+maxconnections];

    int epfd __attribute__ ((__cleanup__(close_fd))) = epoll_create(1+maxconnections);
    if (epfd == -1) {
        printf("Failed to create epoll fd: %s\n", strerror(errno));
        return 1;
    }

    int sockfd __attribute__ ((__cleanup__(close_fd))) = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        return 1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8080),
        .sin_addr = {.s_addr=htonl(INADDR_LOOPBACK)}
    };
    if (bind(sockfd, (struct sockaddr*) &addr, sizeof (struct sockaddr_in)) == -1) {
        printf("Failed to bind to address: %s\n", strerror(errno));
        return 1;
    }

    if (listen(sockfd, 10) == -1) {
        printf("Failed to listen: %s\n", strerror(errno));
        return 1;
    }
    printf("Waiting for incoming connections on http://%s:%d...\n", "localhost", 8080);

    // adding the socket fd so we can be notified of new connections
    struct epoll_event sock_event = (struct epoll_event) {
        .events = EPOLLIN,
        .data = {.fd = sockfd}
    };

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &sock_event) == -1) {
        printf("Failed to add fd to epoll: %s\n", strerror(errno));
        return 1;
    }

    while (1) {
        int numready = epoll_wait(epfd, events, 1 + 2 * numconnections, 10);
        if (numready == -1) {
            printf("Failed to poll: %s.\n", strerror(errno));
            return 1;
        }
        for (int i = 0; i < numready; i++) {
            if (events[i].data.fd == sockfd) {
                if (numconnections >= maxconnections) {
                    printf("Reached max connections...");
                    continue;
                }
                int connfd = accept(sockfd, NULL, NULL);
                if (connfd == -1) {
                    printf("Failed to accept a new connection: %s\n", strerror(errno));
                    return 1;
                }
                // new connection to accept
                struct epoll_event event = (struct epoll_event) {
                    .events = EPOLLIN | EPOLLOUT,
                    .data = {.fd = connfd}
                };
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &event) == -1) {
                    printf("Failed to add fd %d: %s\n", connfd, strerror(errno));
                    return 1;
                }
                numconnections++;
            } else {
                int connfd = events[i].data.fd;
                if (events[i].events & EPOLLIN) {
                    // read & discard the HTTP request
                    char buf[1024];
                    if (read(connfd,buf, 1024) == -1) {
                        printf("Failed to read HTTP request: %s.\n", strerror(errno));
                        return 1;
                    }
                }
                if (events[i].events & EPOLLOUT) {
                    const char* buf = "HTTP/1.1 200 OK\r\nHost: localhost\r\nContent-Length: 12\r\n\r\nHello world!";
                    if (write(connfd, buf, strlen(buf)) == -1) {
                        printf("Failed to write payload: %s.\n", strerror(errno));
                        return 1;
                    }
                    if (epoll_ctl(epfd, EPOLL_CTL_DEL, connfd, NULL) == -1) {
                        printf("Failed to remove fd %d: %s.\n", connfd, strerror(errno));
                        return 1;
                    }
                    if (close (connfd) == -1) {
                        printf("Failed to close connection: %s.\n", strerror(errno));
                        return 1;
                    }
                    numconnections--;
                }
            }
        }
    }
}
