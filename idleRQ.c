#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define PACKETSIZE 1024

typedef struct {
    uint32_t seq;
    char data[PACKETSIZE];
} DataPacket;

typedef struct {
    uint32_t ack;
} AckPacket;

int main(void)
{
    /** declare variable wsa **/
    WSADATA wsa;

    /** declare socket variables – needed for sockets on both client and server **/
    struct sockaddr_in si_other, server;
    SOCKET s;
    int slen = sizeof(si_other);

    /**file variable **/
    unsigned long fileLen;      // length of image file
    FILE *fp;                         // file pointer
    char *buffer;                    // pointer to character array

    /** timer variables **/
    LARGE_INTEGER current, frequency;
    double PCfreq;
    uint64_t StartACK = 0;
    uint64_t ElapsedACK = 0;
    uint64_t MaxDelay = 20000;

    /** other variables **/
    uint32_t seq = 0;
    uint32_t ack = 0;
    int numPackets = 0;
    u_long noBlock = 1;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed. Error Code : %d", WSAGetLastError());
        return 1;
    }
    else {
        printf("\nWINSOCK INITIALIZED");
    }

    /***  OPEN IMAGE FILE AND COPY TO DATA STRUCTURE  ***/
    fp = fopen("C:\\test.jpg", "rb");
    if (fp == NULL) {
        printf("\n Error Opening Image - read");
        fclose(fp);
        exit(0);
    }

    /*** ALLOCATE MEMORY (BUFFER) TO HOLD IMAGE *****/
    fseek(fp,0,SEEK_END);                    //go to EOF
    fileLen = ftell(fp);                             // determine length
    fseek(fp, 0, SEEK_SET);                  //reset fp
    buffer = (char*)malloc(fileLen+1);  //allocated memory

    if(!buffer) {
        printf("\n memory error allocating buffer");
        fclose(fp);
        return 1;
    }

   /*********  READ FILE DATA INTO BUFFER AND CLOSE FILE  *************/
    fread(buffer, fileLen, 1, fp);
    fclose(fp);

    numPackets = (fileLen + PACKETSIZE - 1) / PACKETSIZE;

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

    /*** GET CLOCK INFO ****************/
    QueryPerformanceFrequency(&frequency); // get frequency
    PCfreq = (double)(frequency.QuadPart / 1000.0); // time in ms
    printf("\nPCFreq = %4.9lf and frequency = ", PCfreq);
    printf("%lld", frequency.QuadPart);

    /***** BIND SOCKET ****/
    if (bind(s, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR) {
        printf("Bind failed with error code : %d", WSAGetLastError());
        exit(EXIT_FAILURE);
    }

    puts("\nSERVER SOCKET BIND SUCCESS");

    /***** WAIT FOR DATA ****/
    printf("\nWaiting for data...");

    for(seq = 0; seq < (uint32_t)numPackets; seq++)
    {
        DataPacket pkt;
        int dataLen = (seq == numPackets - 1) ? (fileLen % PACKETSIZE) : PACKETSIZE;
        if(dataLen == 0)
            dataLen = PACKETSIZE;

        pkt.seq = seq;
        memcpy(pkt.data, buffer + seq * PACKETSIZE, dataLen);

        printf("Sending packet seq %u (size = %d)\n", seq, dataLen);

        while(1)
        {
            if(sendto(s, (char*)&pkt, sizeof(uint32_t) + dataLen, 0, (struct sockaddr*)&si_other, slen) == SOCKET_ERROR)
                printf("sendto() failed: %d\n", WSAGetLastError());

            /*** Initialize Start Time ****************/
            QueryPerformanceCounter(&current);
            StartACK = (uint64_t)current.QuadPart;

            while(1)
            {
                AckPacket ackPkt;
                int recv_len = recvfrom(s, (char*)&ackPkt, sizeof(ackPkt), 0, (struct sockaddr*)&si_other, &slen);

                if(recv_len == sizeof(AckPacket))
                {
                    if(ackPkt.ack == seq)
                    {
                        printf("Received ACK for seq %u\n", seq);
                        break;
                    }

                }

                /*** Measure Elapsed Time ****************/
                QueryPerformanceCounter(&current);
                ElapsedACK = (uint64_t)current.QuadPart - StartACK;
                printf(" ACK Elapsed time = ");
                printf("%" PRIu64, ElapsedACK);

                if (ElapsedACK > MaxDelay) {
                    printf("Timeout for seq %u (elapsed ~%llu ms) - resending packet\n", seq, ElapsedACK);
                    break;
                }

            }

            //if(recv_len == sizeof(AckPacket) && ackPkt.ack == seq)
            //    break;
        }
    }


    /* allocate pointer to character array hold image */
    /*buffer = (char *)malloc(fileLen + 1); // allocated memory

    while (waiting_for_data) {
        fflush(stdout);
        memset(packet, '\0', PACKETSIZE); // clear buffer of previously received data

        /* RECEIVE DATA PACKET - blocking *//*
        if ((recv_len = recvfrom(s, buffer, fileLen + 1, 0, (struct sockaddr *)&si_other, &slen)) == SOCKET_ERROR) {
            printf("recvfrom() failed with error code : %d", WSAGetLastError());
            exit(EXIT_FAILURE);
        } else {
            printf("\nSERVER Received packet IPaddr %s Port %d",
                   inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port));
        }








        }
    }*/

    printf("All packets sent successfully.\n");

    free(buffer);
    closesocket(s);
    WSACleanup();

    return 0;
}
