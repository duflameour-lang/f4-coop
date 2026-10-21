// recv.cpp - Stage 3A test bot: acts as a "ghost player".
// Anchors at (first position + 500 X), then replies with a position
// moving in a circle (radius 300) around the anchor.
#include <cstdio>
#include <cmath>
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
    printf("ghost player bot on UDP 31337\n");

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    bool haveAnchor = false;
    double angle = 0.0;

    for (;;)
    {
        PosPacket p = {};
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        int n = recvfrom(s, (char*)&p, sizeof(p), 0,
                         (sockaddr*)&from, &fromLen);
        if (n != sizeof(p) || p.magic != kMagic) continue;

        if (!haveAnchor)
        {
            // skip the pre-load placeholder the plugin emits right after
            // world load (player object exists before being placed)
            if (p.x == 2048.0f && p.y == 2048.0f && p.z == 0.0f)
                continue;
            ax = p.x + 500.0f;
            ay = p.y;
            az = p.z;
            haveAnchor = true;
            printf("anchor set: %.1f %.1f %.1f\n", ax, ay, az);
        }

        angle += 0.15;
        PosPacket r = p;
        r.x = ax + 300.0f * (float)cos(angle);
        r.y = ay + 300.0f * (float)sin(angle);
        r.z = az;
        sendto(s, (const char*)&r, sizeof(r), 0,
               (sockaddr*)&from, fromLen);
        printf("ghost seq=%u pos %.1f %.1f %.1f\n", r.seq, r.x, r.y, r.z);
    }
}