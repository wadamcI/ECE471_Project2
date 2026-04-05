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
#define DEFAULT_TIMEOUT_MS 1000

#define TOTAL_DATA_SIZE 10000
#define MAX_FRAGMENT_PAYLOAD 1400

#define MESSAGE_COUNT 625

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

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int main(int argc, char *argv[]) {
    const char *server_ip = "127.0.0.1";
    int port = DEFAULT_PORT;
    int timeout_ms = DEFAULT_TIMEOUT_MS;

    if (argc >= 2) server_ip = argv[1];
    if (argc >= 3) port = atoi(argv[2]);
    if (argc >= 4) timeout_ms = atoi(argv[3]);

    const uint16_t frag_count =
        (TOTAL_DATA_SIZE + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD;
    const int total_rtt_samples = MESSAGE_COUNT * frag_count;

    uint64_t *frag_rtt_us = malloc((size_t)total_rtt_samples * sizeof(uint64_t));
    if (!frag_rtt_us) {
        perror("malloc frag_rtt_us");
        return 1;
    }

    uint64_t *msg_total_us = malloc((size_t)MESSAGE_COUNT * sizeof(uint64_t));
    if (!msg_total_us) {
        perror("malloc msg_total_us");
        free(frag_rtt_us);
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        free(frag_rtt_us);
        free(msg_total_us);
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
        free(frag_rtt_us);
        free(msg_total_us);
        return 1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) != 1) {
        fprintf(stderr, "invalid IP address: %s\n", server_ip);
        close(sockfd);
        free(frag_rtt_us);
        free(msg_total_us);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        close(sockfd);
        free(frag_rtt_us);
        free(msg_total_us);
        return 1;
    }

    char message[TOTAL_DATA_SIZE];
    for (int i = 0; i < TOTAL_DATA_SIZE; i++) {
        message[i] = (char)('A' + (i % 26));
    }

    int sample_idx = 0;
    int timeouts = 0;
    int failed_messages = 0;

    for (uint32_t msg = 0; msg < MESSAGE_COUNT; msg++) {
        uint64_t msg_t0 = now_us();
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

            pkt.hdr.msg_id = msg;
            pkt.hdr.frag_idx = frag;
            pkt.hdr.frag_count = frag_count;
            pkt.hdr.payload_len = chunk;
            memcpy(pkt.payload, message + offset, chunk);

            size_t send_len = sizeof(FragmentHeader) + chunk;

            for (;;) {
                uint64_t t0 = now_us();

                ssize_t sent = send(sockfd, &pkt, send_len, 0);
                if (sent < 0) {
                    perror("send");
                    failed = 1;
                    break;
                }

                AckPacket ack;
                ssize_t n = recv(sockfd, &ack, sizeof(ack), 0);
                uint64_t t1 = now_us();

                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        timeouts++;
                        continue;   // resend same fragment
                    }
                    perror("recv");
                    failed = 1;
                    break;
                }

                if ((size_t)n == sizeof(ack) &&
                    ack.msg_id == msg &&
                    ack.frag_idx == frag) {
                    frag_rtt_us[sample_idx++] = t1 - t0;
                    break;
                }

                // unexpected ACK, keep waiting/resending
            }

            if (failed) {
                break;
            }
        }

        uint64_t msg_t1 = now_us();
        msg_total_us[msg] = failed ? 0 : (msg_t1 - msg_t0);

        if (failed) {
            failed_messages++;
            while (sample_idx < (int)((msg + 1) * frag_count)) {
                frag_rtt_us[sample_idx++] = 0;
            }
        }
    }

    uint64_t min_frag = UINT64_MAX, max_frag = 0, sum_frag = 0;
    int valid_frag = 0;

    for (int i = 0; i < total_rtt_samples; i++) {
        if (frag_rtt_us[i] == 0) continue;
        if (frag_rtt_us[i] < min_frag) min_frag = frag_rtt_us[i];
        if (frag_rtt_us[i] > max_frag) max_frag = frag_rtt_us[i];
        sum_frag += frag_rtt_us[i];
        valid_frag++;
    }

    uint64_t min_msg = UINT64_MAX, max_msg = 0, sum_msg = 0;
    int valid_msg = 0;

    for (int i = 0; i < MESSAGE_COUNT; i++) {
        if (msg_total_us[i] == 0) continue;
        if (msg_total_us[i] < min_msg) min_msg = msg_total_us[i];
        if (msg_total_us[i] > max_msg) max_msg = msg_total_us[i];
        sum_msg += msg_total_us[i];
        valid_msg++;
    }

    printf("Messages sent              : %d\n", MESSAGE_COUNT);
    printf("Message size (bytes)       : %d\n", TOTAL_DATA_SIZE);
    printf("Fragments per message      : %u\n", frag_count);
    printf("Fragment RTT samples       : %d\n", total_rtt_samples);
    printf("Failed messages            : %d\n", failed_messages);
    printf("Timeouts                   : %d\n\n", timeouts);

    if (valid_frag > 0) {
        printf("Fragment RTT min (us)      : %" PRIu64 "\n", min_frag);
        printf("Fragment RTT max (us)      : %" PRIu64 "\n", max_frag);
        printf("Fragment RTT avg (us)      : %.2f\n\n", (double)sum_frag / valid_frag);
    }

    if (valid_msg > 0) {
        printf("Message total min (us)     : %" PRIu64 "\n", min_msg);
        printf("Message total max (us)     : %" PRIu64 "\n", max_msg);
        printf("Message total avg (us)     : %.2f\n\n", (double)sum_msg / valid_msg);
    }

    printf("Fragment RTT samples (us):\n");
    for (int i = 0; i < total_rtt_samples; i++) {
        printf("%" PRIu64 "\n", frag_rtt_us[i]);
    }

    close(sockfd);
    free(frag_rtt_us);
    free(msg_total_us);
    return 0;
}
