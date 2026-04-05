#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>

#define DEFAULT_PORT 5001
#define DEFAULT_COUNT 1000
#define DEFAULT_TIMEOUT_MS 1000

#define TOTAL_DATA_SIZE 10000
#define MAX_FRAGMENT_PAYLOAD 1400

typedef struct __attribute__((packed)) {
    uint32_t msg_id;         // logical message number
    uint16_t frag_idx;       // fragment index
    uint16_t frag_count;     // total number of fragments
    uint16_t payload_len;    // bytes in this fragment
} FragmentHeader;

typedef struct __attribute__((packed)) {
    FragmentHeader hdr;
    char payload[MAX_FRAGMENT_PAYLOAD];
} FragmentPacket;

typedef struct __attribute__((packed)) {
    uint32_t msg_id;
    uint16_t frag_idx;
} AckPacket;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int main(int argc, char *argv[]) {
    const char *server_ip = "127.0.0.1";
    int port = DEFAULT_PORT;
    int count = DEFAULT_COUNT;
    int timeout_ms = DEFAULT_TIMEOUT_MS;

    if (argc >= 2) server_ip = argv[1];
    if (argc >= 3) port = atoi(argv[2]);
    if (argc >= 4) count = atoi(argv[3]);
    if (argc >= 5) timeout_ms = atoi(argv[4]);

    if (count <= 0) {
        fprintf(stderr, "count must be > 0\n");
        return 1;
    }

    const uint16_t frag_count =
        (TOTAL_DATA_SIZE + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD;

    uint64_t *rtt_us = malloc((size_t)count * sizeof(uint64_t));
    if (!rtt_us) {
        perror("malloc");
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        free(rtt_us);
        return 1;
    }

    int bufsize = 1 << 20;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sockfd);
        free(rtt_us);
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) != 1) {
        fprintf(stderr, "invalid IP address: %s\n", server_ip);
        close(sockfd);
        free(rtt_us);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        close(sockfd);
        free(rtt_us);
        return 1;
    }

    char message[TOTAL_DATA_SIZE];
    for (int i = 0; i < TOTAL_DATA_SIZE; i++) {
        message[i] = (char)('A' + (i % 26));
    }

    int received = 0;
    int timeouts = 0;

    for (int msg = 0; msg < count; msg++) {
        uint64_t t0 = now_us();
        int failed = 0;

        for (uint16_t frag = 0; frag < frag_count; frag++) {
            FragmentPacket pkt;
            memset(&pkt, 0, sizeof(pkt));

            size_t offset = (size_t)frag * MAX_FRAGMENT_PAYLOAD;
            uint16_t chunk = (uint16_t)(
                (TOTAL_DATA_SIZE - (int)offset > MAX_FRAGMENT_PAYLOAD)
                ? MAX_FRAGMENT_PAYLOAD
                : (TOTAL_DATA_SIZE - (int)offset)
            );

            pkt.hdr.msg_id = (uint32_t)msg;
            pkt.hdr.frag_idx = frag;
            pkt.hdr.frag_count = frag_count;
            pkt.hdr.payload_len = chunk;
            memcpy(pkt.payload, message + offset, chunk);

            size_t send_len = sizeof(FragmentHeader) + chunk;

            for (;;) {
                ssize_t sent = send(sockfd, &pkt, send_len, 0);
                if (sent < 0) {
                    perror("send");
                    failed = 1;
                    break;
                }

                AckPacket ack;
                ssize_t n = recv(sockfd, &ack, sizeof(ack), 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        timeouts++;
                        continue; // resend same fragment
                    }
                    perror("recv");
                    failed = 1;
                    break;
                }

                if ((size_t)n == sizeof(ack) &&
                    ack.msg_id == (uint32_t)msg &&
                    ack.frag_idx == frag) {
                    break; // next fragment
                }
            }

            if (failed) break;
        }

        uint64_t t1 = now_us();

        if (!failed) {
            rtt_us[msg] = t1 - t0;
            received++;
        } else {
            rtt_us[msg] = 0;
        }
    }

    uint64_t min_us = UINT64_MAX, max_us = 0, sum_us = 0;
    int valid = 0;

    for (int i = 0; i < count; i++) {
        if (rtt_us[i] == 0) continue;
        if (rtt_us[i] < min_us) min_us = rtt_us[i];
        if (rtt_us[i] > max_us) max_us = rtt_us[i];
        sum_us += rtt_us[i];
        valid++;
    }

    printf("Logical messages sent : %d\n", count);
    printf("Message size (bytes)  : %d\n", TOTAL_DATA_SIZE);
    printf("Fragments/message     : %u\n", frag_count);
    printf("Messages received     : %d\n", received);
    printf("Timeouts              : %d\n", timeouts);

    if (valid > 0) {
        printf("RTT min (us)          : %" PRIu64 "\n", min_us);
        printf("RTT max (us)          : %" PRIu64 "\n", max_us);
        printf("RTT avg (us)          : %.2f\n", (double)sum_us / (double)valid);
    } else {
        printf("No valid RTT samples collected.\n");
    }

    FILE *fp = fopen("rtt_frag_10k.csv", "w");
    if (fp) {
        fprintf(fp, "msg_id,rtt_us\n");
        for (int i = 0; i < count; i++) {
            fprintf(fp, "%d,%" PRIu64 "\n", i, rtt_us[i]);
        }
        fclose(fp);
        printf("Saved samples to rtt_frag_10k.csv\n");
    }

    close(sockfd);
    free(rtt_us);
    return 0;
}
