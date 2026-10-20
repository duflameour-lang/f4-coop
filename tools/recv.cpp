// recv.cpp - Stage 2 test receiver: prints f4-coop position packets.
#include <cstdio>
#include <winsock2.h>
#include <ws2tcpip.h>

struct PosPacket
{
    unsigned int magic;
    unsigned int seq;
    float x, y, z;
};
static const unsigned int kMagic = 0x46344350u; // "F4CP"

int main()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("WSAStartup failed\n");
        return 1;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(31337);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0)
    {
        printf("bind failed: %d\n", WSAGetLastError());
        return 1;
    }
    printf("listening on UDP 31337...\n");
    for (;;)
    {
        PosPacket p = {};
        int n = recv(s, (char*)&p, sizeof(p), 0);
        if (n == sizeof(p) && p.magic == kMagic)
            printf("seq=%u pos %.1f %.1f %.1f\n", p.seq, p.x, p.y, p.z);
        else
            printf("bad packet (%d bytes)\n", n);
    }
}