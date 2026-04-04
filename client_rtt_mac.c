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

#define PAYLOAD_SIZE 64
#define DEFAULT_PORT 5001
#define DEFAULT_COUNT 2000
#define DEFAULT_TIMEOUT_MS 1000

typedef struct {
    uint32_t seq;
    char payload[PAYLOAD_SIZE];
} Packet;

typedef struct {
    uint32_t seq;
} Ack;

static uint64_t now_us(void) {
#if defined(__APPLE__)
    // CLOCK_MONOTONIC is available on modern macOS
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#elif defined(CLOCK_MONOTONIC_RAW)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
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

    // Optional: larger buffers, not critical here
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

    // connect() on UDP is useful: lets us use send/recv and filters other sources
    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        close(sockfd);
        free(rtt_us);
        return 1;
    }

    Packet pkt;
    Ack ack;
    memset(&pkt, 0, sizeof(pkt));

    int received = 0;
    int timeouts = 0;

    for (int i = 0; i < count; i++) {
        pkt.seq = (uint32_t)i;

        uint64_t t0 = now_us();

        ssize_t sent = send(sockfd, &pkt, sizeof(pkt), 0);
        if (sent < 0) {
            perror("send");
            rtt_us[i] = 0;
            continue;
        }

        ssize_t n = recv(sockfd, &ack, sizeof(ack), 0);
        uint64_t t1 = now_us();

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                timeouts++;
            } else {
                perror("recv");
            }
            rtt_us[i] = 0;
            continue;
        }

        if ((size_t)n != sizeof(ack) || ack.seq != (uint32_t)i) {
            rtt_us[i] = 0;
            continue;
        }

        rtt_us[i] = t1 - t0;
        received++;
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

    printf("Packets requested : %d\n", count);
    printf("ACKs received     : %d\n", received);
    printf("Timeouts/errors   : %d\n", timeouts + (count - received - timeouts));
    if (valid > 0) {
        printf("RTT min (us)      : %" PRIu64 "\n", min_us);
        printf("RTT max (us)      : %" PRIu64 "\n", max_us);
        printf("RTT avg (us)      : %.2f\n", (double)sum_us / (double)valid);
    } else {
        printf("No valid RTT samples collected.\n");
    }

    // Optional CSV dump after measurement loop
    FILE *fp = fopen("rtt_samples.csv", "w");
    if (fp) {
        fprintf(fp, "seq,rtt_us\n");
        for (int i = 0; i < count; i++) {
            fprintf(fp, "%d,%" PRIu64 "\n", i, rtt_us[i]);
        }
        fclose(fp);
        printf("Saved samples to rtt_samples.csv\n");
    }

    close(sockfd);
    free(rtt_us);
    return 0;
}
