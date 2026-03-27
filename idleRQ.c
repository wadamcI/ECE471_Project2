#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#define PACKETSIZE 1024

int main(void)
{
    /** declare variable wsa **/
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed. Error Code : %d", WSAGetLastError());
        return 1;
    } else {
        printf("\nWINSOCK INITIALIZED");
    }

    /** declare socket variables – needed for sockets on both client and server **/
    struct sockaddr_in si_other, server;
    SOCKET s;
    int slen = sizeof(si_other);

    char *buffer = NULL;
    char packet[PACKETSIZE];
    int recv_len;
    int waiting_for_data = 1;
    int fileLen = PACKETSIZE;
    u_long noBlock = 1;

    /***** CREATE CLIENT SOCKET ****/
    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
        printf("Could not create socket : %d", WSAGetLastError());
    }
    printf("UDP CLIENT SOCKET CREATED.\n");

    /***** INITIALIZE SOCKET STRUCT - Non Blocking Client ****/
    ioctlsocket(s, FIONBIO, &noBlock);
    si_other.sin_addr.s_addr = inet_addr("127.0.0.1");
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(80);

    if (sendto(s, buffer, sizeof(buffer), 0, (struct sockaddr *)&si_other, slen) == SOCKET_ERROR) {
        printf("sendto() failed with error code : %d", WSAGetLastError());
        exit(EXIT_FAILURE);
    } else {
        printf("\nsent buffer");
    }

    /***** CREATE SERVER SOCKET ****/
    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
        printf("Could not create socket : %d", WSAGetLastError());
    } else {
        printf("\nUDP SERVER SOCKET CREATED");
    }

    server.sin_addr.s_addr = inet_addr("127.0.0.1"); // or INADDR_ANY
    server.sin_family = AF_INET;
    server.sin_port = htons(80);

    /***** BIND SOCKET ****/
    if (bind(s, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR) {
        printf("Bind failed with error code : %d", WSAGetLastError());
        exit(EXIT_FAILURE);
    }

    puts("\nSERVER SOCKET BIND SUCCESS");

    /***** WAIT FOR DATA ****/
    printf("\nWaiting for data...");

    /** allocate pointer to character array hold image **/
    buffer = (char *)malloc(fileLen + 1); // allocated memory

    while (waiting_for_data) {
        fflush(stdout);
        memset(packet, '\0', PACKETSIZE); // clear buffer of previously received data

        /******** RECEIVE DATA PACKET - blocking *************/
        if ((recv_len = recvfrom(s, buffer, fileLen + 1, 0, (struct sockaddr *)&si_other, &slen)) == SOCKET_ERROR) {
            printf("recvfrom() failed with error code : %d", WSAGetLastError());
            exit(EXIT_FAILURE);
        } else {
            printf("\nSERVER Received packet IPaddr %s Port %d",
                   inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port));
        }

        /** timer variables **/
        LARGE_INTEGER current, frequency;
        double PCfreq;
        uint64_t StartACK = 0;
        uint64_t ElapsedACK = 0;
        uint64_t MaxDelay = 20000;

        /*** GET CLOCK INFO ****************/
        QueryPerformanceFrequency(&frequency); // get frequency
        PCfreq = (double)(frequency.QuadPart / 1000.0); // time in ms
        printf("\nPCFreq = %4.9lf and frequency = ", PCfreq);
        printf("%lld", frequency.QuadPart);

        /*** Initialize Start Time ****************/
        QueryPerformanceCounter(&current);
        StartACK = (uint64_t)current.QuadPart;

        /*** Measure Elapsed Time ****************/
        QueryPerformanceCounter(&current);
        ElapsedACK = (uint64_t)current.QuadPart - StartACK;
        printf(" ACK Elapsed time = ");
        printf("%" PRIu64, ElapsedACK);

        if (ElapsedACK > MaxDelay) {
            break;
        }
    }

    free(buffer);
    closesocket(s);
    WSACleanup();

    return 0;
}