// client.cpp - Stage 6 test bot: a FAKE second player for host-relay tests.
// Sends its own circling position (default id=2) to the host at 10 Hz and
// anchors its circle on the first position the host broadcasts. Lets us
// validate the full host->clients relay on ONE machine before LAN.
// Usage: client.exe [host_ip] [id]
#include <cstdio>
#include <cmath>
#include <cstdlib>
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

int main(int argc, char** argv)
{
    const char* hostIp = argc > 1 ? argv[1] : "127.0.0.1";
    unsigned int myId = argc > 2
        ? (unsigned int)strtoul(argv[2], nullptr, 10) : 2;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("WSAStartup failed\n");
        return 1;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in self = {};
    self.sin_family = AF_INET;
    self.sin_port = 0;
    self.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&self, sizeof(self)) != 0)
    {
        printf("bind failed: %d\n", WSAGetLastError());
        return 1;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    sockaddr_in host = {};
    host.sin_family = AF_INET;
    host.sin_port = htons(31337);
    inet_pton(AF_INET, hostIp, &host.sin_addr);

    printf("fake player id=%u -> %s:31337\n", myId, hostIp);

    float ax = 0.f, ay = 0.f, az = 0.f;
    bool anchored = false;
    double angle = 0.0;
    unsigned int seq = 0;

    for (;;)
    {
        // receive whatever the host broadcasts
        for (int i = 0; i < 16; i++)
        {
            PosPacket p = {};
            sockaddr_in from = {};
            int fromLen = sizeof(from);
            int n = recvfrom(s, (char*)&p, sizeof(p), 0,
                             (sockaddr*)&from, &fromLen);
            if (n != sizeof(p)) break; // nothing more
            if (p.magic != kMagic || p.playerId == myId) continue;
            printf("recv id=%u seq=%u pos %.1f %.1f %.1f\n",
                   p.playerId, p.seq, p.x, p.y, p.z);
            if (!anchored)
            {
                ax = p.x + 600.0f;
                ay = p.y;
                az = p.z;
                anchored = true;
                printf("anchored at %.1f %.1f %.1f\n", ax, ay, az);
            }
        }

        // send our own circling position (garbage until anchored - the
        // host's distance guard ignores it, then real positions flow)
        angle += 0.12;
        PosPacket out = {};
        out.magic = kMagic;
        out.seq = seq++;
        out.playerId = myId;
        out.x = ax + 200.0f * (float)cos(angle);
        out.y = ay + 200.0f * (float)sin(angle);
        out.z = az;
        sendto(s, (const char*)&out, sizeof(out), 0,
               (sockaddr*)&host, sizeof(host));

        Sleep(100);
    }
}
