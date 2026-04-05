#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#define DEFAULT_PORT 5001
#define TOTAL_DATA_SIZE 10000
#define MAX_FRAGMENT_PAYLOAD 1400
#define MAX_FRAGMENTS ((TOTAL_DATA_SIZE + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD)

typedef struct __attribute__((packed)) {
    uint32_t msg_id;
    uint16_t frag_idx;
    uint16_t frag_count;
    uint16_t payload_len;
} FragmentHeader;

typedef struct __attribute__((packed)) {
    FragmentHeader hdr;
    char payload[MAX_FRAGMENT_PAYLOAD];
} FragmentPacket;

typedef struct __attribute__((packed)) {
    uint32_t msg_id;
    uint16_t frag_idx;
} AckPacket;

typedef struct {
    uint32_t current_msg_id;
    int active;
    uint16_t frag_count;
    uint16_t received_count;
    uint8_t received[MAX_FRAGMENTS];
    char data[TOTAL_DATA_SIZE];
} ReassemblyBuffer;

static void reset_buffer(ReassemblyBuffer *rb) {
    rb->current_msg_id = 0;
    rb->active = 0;
    rb->frag_count = 0;
    rb->received_count = 0;
    memset(rb->received, 0, sizeof(rb->received));
    memset(rb->data, 0, sizeof(rb->data));
}

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

    int bufsize = 1 << 20;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

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

    ReassemblyBuffer rb;
    reset_buffer(&rb);

    printf("UDP fragmented server listening on port %d\n", port);

    for (;;) {
        FragmentPacket pkt;
        ssize_t n = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&cliaddr, &clilen);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }

        if ((size_t)n < sizeof(FragmentHeader)) {
            continue;
        }

        uint32_t msg_id = pkt.hdr.msg_id;
        uint16_t frag_idx = pkt.hdr.frag_idx;
        uint16_t frag_count = pkt.hdr.frag_count;
        uint16_t payload_len = pkt.hdr.payload_len;

        if (frag_count == 0 || frag_count > MAX_FRAGMENTS) {
            continue;
        }
        if (frag_idx >= frag_count) {
            continue;
        }
        if (payload_len > MAX_FRAGMENT_PAYLOAD) {
            continue;
        }

        size_t expected_len = sizeof(FragmentHeader) + payload_len;
        if ((size_t)n != expected_len) {
            continue;
        }

        if (!rb.active || rb.current_msg_id != msg_id) {
            reset_buffer(&rb);
            rb.active = 1;
            rb.current_msg_id = msg_id;
            rb.frag_count = frag_count;
        }

        if (frag_count != rb.frag_count) {
            continue;
        }

        size_t offset = (size_t)frag_idx * MAX_FRAGMENT_PAYLOAD;
        if (offset + payload_len <= TOTAL_DATA_SIZE) {
            if (!rb.received[frag_idx]) {
                memcpy(rb.data + offset, pkt.payload, payload_len);
                rb.received[frag_idx] = 1;
                rb.received_count++;
            }
        }

        AckPacket ack;
        ack.msg_id = msg_id;
        ack.frag_idx = frag_idx;

        ssize_t sent = sendto(sockfd, &ack, sizeof(ack), 0,
                              (struct sockaddr *)&cliaddr, clilen);
        if (sent < 0) {
            perror("sendto");
            break;
        }

        if (rb.received_count == rb.frag_count) {
            printf("Reassembled message %" PRIu32 " (%d bytes, %u fragments)\n",
                   rb.current_msg_id, TOTAL_DATA_SIZE, rb.frag_count);
            reset_buffer(&rb);
        }
    }

    close(sockfd);
    return 0;
}
