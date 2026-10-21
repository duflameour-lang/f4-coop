// f4-coop - Stage 3, step B: network-controlled self position via the
// engine's own move (MoveRefrToPosition, the native behind Papyrus MoveTo),
// applied on the game thread through F4SE's task interface.
// Target: Fallout 4 1.10.163.0, F4SE 0.6.23.
typedef unsigned char UInt8;
typedef unsigned int UInt32;
typedef unsigned long long UInt64;

#include "f4se/PluginAPI.h"
#include "f4se_common/f4se_version.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// after windows.h: GameThreads.h needs CRITICAL_SECTION/HANDLE
#include "f4se/GameThreads.h" // ITaskDelegate

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <thread>

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;
static F4SETaskInterface* g_tasks = nullptr;
static std::atomic<bool> g_worldLoaded{false};
static uintptr_t g_gameBase = 0;

// Offsets verified against ianpatt/f4se tag v0.6.23 (RelocAddr/RelocPtr
// convention: runtime address = module base + offset).
static const uintptr_t kPlayerPtrOffset     = 0x05AA4388; // g_player
static const uintptr_t kParentCellOffset    = 0xB8;       // TESObjectREFR::parentCell
static const uintptr_t kPosOffset           = 0xD0;       // TESObjectREFR::pos
static const uintptr_t kInvalidHandleOffset = 0x038CCE04; // g_invalidRefHandle
static const uintptr_t kMoveRefrOffset      = 0x013FE7E0; // MoveRefrToPosition
static const uintptr_t kGetWorldspaceOffset = 0x0040F290; // TESObjectREFR::GetWorldspace

typedef void (*_MoveRefrToPosition)(void* source, UInt32* pTargetHandle,
    void* parentCell, void* worldSpace, float* position, float* rotation);
typedef void* (*_GetWorldspace)(void* refr);

// ---- logging ------------------------------------------------------------
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

    // bind to an arbitrary source port so replies can come back to us
    sockaddr_in self = {};
    self.sin_family = AF_INET;
    self.sin_port = 0;
    self.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (sockaddr*)&self, sizeof(self)) != 0) return false;

    // non-blocking receive
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);

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

// ---- ghost state: net thread writes, game thread applies ----------------
struct GhostState
{
    UInt64 version; // bumped on every store, 0 = nothing yet
    float x, y, z;
};
static std::mutex g_ghostLock;
static GhostState g_ghost = {};
static std::atomic<bool> g_taskInFlight{false};

static void udpTryRecv()
{
    if (g_sock == INVALID_SOCKET) return;
    static float s_lastRxX = 1e30f;
    for (int i = 0; i < 10; i++)
    {
        PosPacket p = {};
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        int n = recvfrom(g_sock, (char*)&p, sizeof(p), 0,
                         (sockaddr*)&from, &fromLen);
        if (n != sizeof(p)) break; // nothing more (WSAEWOULDBLOCK)
        if (p.magic != kMagic) continue;

        if (p.x != s_lastRxX)
        {
            logline("[f4-coop] rx seq=%u pos %.1f %.1f %.1f",
                    p.seq, p.x, p.y, p.z);
            s_lastRxX = p.x;
        }

        // Stage 3B: the net thread only records the latest ghost position;
        // the actual move happens on the game thread (ApplyGhostMoveTask).
        {
            std::lock_guard<std::mutex> lk(g_ghostLock);
            g_ghost.version++;
            g_ghost.x = p.x; g_ghost.y = p.y; g_ghost.z = p.z;
        }
    }
}

// Runs on the game thread: F4SE pumps tasks from a hook inside the engine's
// message queue processing (Hooks_Threads.cpp), i.e. the main update loop.
// Call pattern mirrors f4se/PapyrusObjectReference.cpp ConnectObjects:
// nullHandle + own parentCell + GetWorldspace().
class ApplyGhostMoveTask : public ITaskDelegate
{
public:
    void Run() override
    {
        g_taskInFlight = false; // a fresh task may be posted from here on

        GhostState g;
        {
            std::lock_guard<std::mutex> lk(g_ghostLock);
            g = g_ghost;
        }
        if (g.version == 0) return;
        if (fabsf(g.x) > 1e6f || fabsf(g.y) > 1e6f || fabsf(g.z) > 1e6f)
            return; // sanity: never teleport to garbage

        void* player = *(void**)(g_gameBase + kPlayerPtrOffset);
        if (!player) return;
        void* parentCell = *(void**)((char*)player + kParentCellOffset);
        if (!parentCell) return;

        _GetWorldspace getWorldspace =
            (_GetWorldspace)(g_gameBase + kGetWorldspaceOffset);
        void* worldspace = getWorldspace(player);

        // face the direction of movement (euler Z rotation, radians)
        float cur[3];
        memcpy(cur, (char*)player + kPosOffset, sizeof(cur));
        float rot[3] = { 0.f, 0.f, atan2f(g.y - cur[1], g.x - cur[0]) };
        float pos[3] = { g.x, g.y, g.z };

        UInt32 nullHandle = *(UInt32*)(g_gameBase + kInvalidHandleOffset);
        _MoveRefrToPosition moveRefr =
            (_MoveRefrToPosition)(g_gameBase + kMoveRefrOffset);

        static std::atomic<UInt64> s_moves{0};
        UInt64 n = ++s_moves;
        bool logIt = (n <= 5 || n % 50 == 0);
        if (logIt)
            logline("[f4-coop] moveRefr #%llu -> %.1f %.1f %.1f (cell=%p ws=%p)",
                    (unsigned long long)n, g.x, g.y, g.z, parentCell, worldspace);

        moveRefr(player, &nullHandle, parentCell, worldspace, pos, rot);

        if (logIt)
        {
            float after[3];
            memcpy(after, (char*)player + kPosOffset, sizeof(after));
            logline("[f4-coop] move #%llu done, pos now %.1f %.1f %.1f",
                    (unsigned long long)n, after[0], after[1], after[2]);
        }
    }
};
// -------------------------------------------------------------------------

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
    UInt64 lastPosted = 0;
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
        udpTryRecv();

        // post at most one apply task at a time, and only on fresh data
        UInt64 version;
        {
            std::lock_guard<std::mutex> lk(g_ghostLock);
            version = g_ghost.version;
        }
        if (version != lastPosted && g_tasks &&
            !g_taskInFlight.exchange(true))
        {
            lastPosted = version;
            g_tasks->AddTask(new ApplyGhostMoveTask());
        }

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
        info->version     = 6;

        if (f4se->runtimeVersion != RUNTIME_VERSION_1_10_163)
        {
            logline("[f4-coop] refuse: runtime 0x%08X, want 1.10.163",
                    f4se->runtimeVersion);
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

        g_tasks = (F4SETaskInterface*)f4se->QueryInterface(kInterface_Task);
        if (!g_tasks)
            logline("[f4-coop] no task interface - ghost apply disabled");

        if (!udpInit())
            logline("[f4-coop] udp init FAILED (wsa=%d)", WSAGetLastError());
        else
            logline("[f4-coop] udp bound, sending to 127.0.0.1:%u", kPort);

        std::thread t(positionThreadMain);
        t.detach();
        logline("[f4-coop] loaded v6, handle=%u runtime=0x%08X",
                g_pluginHandle, f4se->runtimeVersion);
        return true;
    }
}
