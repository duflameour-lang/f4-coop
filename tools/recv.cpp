// recv.cpp - Stage 5 test bot: simulates TWO network players ("ghosts").
// Anchors each player at (first real position + offset), then replies with
// positions moving in circles (different anchor/radius/direction per player).
#include <cstdio>
#include <cmath>
#include <winsock2.h>
#include <ws2tcpip.h>

struct PosPacket
{
    unsigned int magic;
    unsigned int seq;
    unsigned int playerId;
    float x, y, z;
};
static const unsigned int kMagic = 0x46344350u; // "F4CP"

struct VPlayer
{
    unsigned int id;
    float ax, ay, az; // anchor
    float radius;
    float speed;      // radians per packet (sign = direction)
    double angle;
    unsigned int seq;
};

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
    printf("ghost players bot on UDP 31337 (players 101, 102)\n");

    VPlayer players[2] = {};
    players[0].id = 101;
    players[0].radius = 300.0f;
    players[0].speed = 0.15f;  // ccw
    players[1].id = 102;
    players[1].radius = 250.0f;
    players[1].speed = -0.10f; // cw
    bool haveAnchor = false;

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
            players[0].ax = p.x + 500.0f;
            players[0].ay = p.y;
            players[1].ax = p.x;
            players[1].ay = p.y + 500.0f;
            players[0].az = players[1].az = p.z;
            players[0].angle = 0.0;
            players[1].angle = 1.5;
            haveAnchor = true;
            printf("anchor set: p101 (%.1f %.1f), p102 (%.1f %.1f)\n",
                   players[0].ax, players[0].ay,
                   players[1].ax, players[1].ay);
        }

        for (auto& vp : players)
        {
            vp.angle += vp.speed;
            PosPacket r = {};
            r.magic = kMagic;
            r.seq = vp.seq++;
            r.playerId = vp.id;
            r.x = vp.ax + vp.radius * (float)cos(vp.angle);
            r.y = vp.ay + vp.radius * (float)sin(vp.angle);
            r.z = vp.az;
            sendto(s, (const char*)&r, sizeof(r), 0,
                   (sockaddr*)&from, fromLen);
            printf("player %u seq=%u pos %.1f %.1f %.1f\n",
                   r.playerId, r.seq, r.x, r.y, r.z);
        }
    }
}
