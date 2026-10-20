// f4-coop - Stage 2, step 1: send player position over UDP (transport test).
// Target: Fallout 4 1.10.163.0, F4SE 0.6.23.
typedef unsigned int UInt32;
typedef unsigned long long UInt64;

#include "f4se/PluginAPI.h"
#include "f4se_common/f4se_version.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <thread>

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;
static std::atomic<bool> g_worldLoaded{false};
static uintptr_t g_gameBase = 0;

// Offsets verified against ianpatt/f4se tag v0.6.23.
static const uintptr_t kPlayerPtrOffset = 0x05AA4388;
static const uintptr_t kPosOffset = 0xD0;

// ---- UDP transport ------------------------------------------------------
static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_dest = {};
static unsigned int g_seq = 0;

struct PosPacket
{
    unsigned int magic;
    unsigned int seq;
    float x, y, z;
};
static const unsigned int kMagic = 0x46344350u; // "F4CP"
static const unsigned short kPort = 31337;

static bool udpInit()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &g_dest.sin_addr);
    return true;
}

static void udpSendPos(float x, float y, float z)
{
    if (g_sock == INVALID_SOCKET) return;
    PosPacket p;
    p.magic = kMagic;
    p.seq = g_seq++;
    p.x = x; p.y = y; p.z = z;
    sendto(g_sock, (const char*)&p, sizeof(p), 0,
           (sockaddr*)&g_dest, sizeof(g_dest));
}
// -------------------------------------------------------------------------

static void logline(const char* fmt, ...)
{
    FILE* f = nullptr;
    fopen_s(&f, "f4coop.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    va_end(ap);
    fclose(f);
}

static void onF4SEMessage(F4SEMessagingInterface::Message* msg)
{
    if (msg->type == F4SEMessagingInterface::kMessage_GameDataReady && msg->data)
        logline("[f4-coop] GameDataReady");
    if (msg->type == F4SEMessagingInterface::kMessage_GameLoaded)
    {
        logline("[f4-coop] GameLoaded");
        g_worldLoaded = true;
    }
}

static void positionThreadMain()
{
    float lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
    bool haveLast = false;
    for (;;)
    {
        Sleep(100);
        if (!g_worldLoaded) continue;
        void* player = *(void**)(g_gameBase + kPlayerPtrOffset);
        if (!player) continue;
        float x = *(float*)((char*)player + kPosOffset);
        float y = *(float*)((char*)player + kPosOffset + 4);
        float z = *(float*)((char*)player + kPosOffset + 8);

        udpSendPos(x, y, z);

        float dx = x - lastX, dy = y - lastY, dz = z - lastZ;
        bool moved = !haveLast || (fabsf(dx) + fabsf(dy) + fabsf(dz)) > 0.5f;
        if (moved)
        {
            logline("[f4-coop] pos %.1f %.1f %.1f", x, y, z);
            lastX = x; lastY = y; lastZ = z;
            haveLast = true;
        }
    }
}

extern "C"
{
    __declspec(dllexport)
    bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name        = "f4-coop";
        info->version     = 3;

        if (f4se->runtimeVersion != RUNTIME_VERSION_1_10_163)
        {
            logline("[f4-coop] refuse: runtime 0x%08X, want 1.10.163", f4se->runtimeVersion);
            return false;
        }
        return true;
    }

    __declspec(dllexport)
    bool F4SEPlugin_Load(const F4SEInterface* f4se)
    {
        g_pluginHandle = f4se->GetPluginHandle();
        g_messaging = (F4SEMessagingInterface*)f4se->QueryInterface(kInterface_Messaging);
        if (!g_messaging)
        {
            logline("[f4-coop] no messaging interface");
            return false;
        }
        g_messaging->RegisterListener(g_pluginHandle, "F4SE", onF4SEMessage);
        g_gameBase = (uintptr_t)GetModuleHandle(NULL);

        if (!udpInit())
            logline("[f4-coop] udp init FAILED (wsa=%d)", WSAGetLastError());
        else
            logline("[f4-coop] udp -> 127.0.0.1:%u", kPort);

        std::thread t(positionThreadMain);
        t.detach();
        logline("[f4-coop] loaded v3, handle=%u runtime=0x%08X",
                g_pluginHandle, f4se->runtimeVersion);
        return true;
    }
}