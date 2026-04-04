#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#define PAYLOAD_SIZE 64
#define DEFAULT_PORT 5001

typedef struct {
    uint32_t seq;
    char payload[PAYLOAD_SIZE];
} Packet;

typedef struct {
    uint32_t seq;
} Ack;

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in servaddr, cliaddr;
    socklen_t clilen = sizeof(cliaddr);
    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons((uint16_t)port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    Packet pkt;
    Ack ack;

    for (;;) {
        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&cliaddr, &clilen);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }

        ack.seq = pkt.seq;

        ssize_t sent = sendto(sockfd, &ack, sizeof(ack), 0,
                              (struct sockaddr *)&cliaddr, clilen);
        if (sent < 0) {
            perror("sendto");
            break;
        }
    }

    close(sockfd);
    return 0;
}
