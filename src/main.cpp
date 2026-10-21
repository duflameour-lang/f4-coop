// f4-coop - Stage 3, step B: network-controlled self position via the
// engine's own move (MoveRefrToPosition, the native behind Papyrus MoveTo),
// applied on the game thread through F4SE's task interface.
// v7: moves are gated on the save-load lifecycle (PostLoadGame arms,
// PreLoadGame disarms) and on UI pause mode (loading screens, Pip-Boy):
// applying moves during save load hangs the load screen forever.
// v8: PostLoadGame bool is value-encoded in msg->data - never dereference
// it (v7 crashed the game on every save load).
// v9: after PostLoadGame, applying waits out a grace period: the loading
// screen is not detected by UI::numPauseGame (v8 log: zero blocked lines,
// moves ran while the load screen was stuck), and moving the player during
// load restarts cell streaming and hangs the screen forever.
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
static std::atomic<bool> g_applyArmed{false}; // armed by PostLoadGame
static std::atomic<unsigned long long> g_armedAtTick{0}; // GetTickCount64
static uintptr_t g_gameBase = 0;

// Offsets verified against ianpatt/f4se tag v0.6.23 (RelocAddr/RelocPtr
// convention: runtime address = module base + offset).
static const uintptr_t kPlayerPtrOffset     = 0x05AA4388; // g_player
static const uintptr_t kParentCellOffset    = 0xB8;       // TESObjectREFR::parentCell
static const uintptr_t kPosOffset           = 0xD0;       // TESObjectREFR::pos
static const uintptr_t kInvalidHandleOffset = 0x038CCE04; // g_invalidRefHandle
static const uintptr_t kMoveRefrOffset      = 0x013FE7E0; // MoveRefrToPosition
static const uintptr_t kGetWorldspaceOffset = 0x0040F290; // TESObjectREFR::GetWorldspace
static const uintptr_t kUiSingletonOffset   = 0x058D0898; // g_ui
static const uintptr_t kUiPauseCountOffset  = 0x1E0;      // UI::numPauseGame

// safety: never drag the player farther than this in one move (units)
static const float kMaxDragDist = 5000.0f;

// after PostLoadGame, wait this long before applying any move: the load
// must finish undisturbed or the loading screen never dismisses
static const unsigned kArmGraceMs = 10000;

typedef void (*_MoveRefrToPosition)(void* source, UInt32* pTargetHandle,
    void* parentCell, void* worldSpace, float* position, float* rotation);
typedef void* (*_GetWorldspace)(void* refr);

// ---- logging ------------------------------------------------------------
static void logline(const char* fmt, ...)
{
    FILE* f = nullptr;
    fopen_s(&f, "f4coop.log", "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02u:%02u:%02u.%03u ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
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

        if (!g_applyArmed) return; // no save load completed yet

        // grace: the load must finish undisturbed before the first move
        // (moving during the load screen hangs it forever)
        if (GetTickCount64() - g_armedAtTick.load() < kArmGraceMs)
            return;

        GhostState g;
        {
            std::lock_guard<std::mutex> lk(g_ghostLock);
            g = g_ghost;
        }
        if (g.version == 0) return;

        void* player = *(void**)(g_gameBase + kPlayerPtrOffset);
        if (!player) return;
        void* parentCell = *(void**)((char*)player + kParentCellOffset);
        if (!parentCell) return;

        // never move while a menu/loading screen is up (UI::numPauseGame,
        // "isInMenuMode" per GameMenus.h). Moving the player during save
        // load hangs the loading screen forever (observed in v6).
        void* ui = *(void**)(g_gameBase + kUiSingletonOffset);
        if (!ui) return;
        UInt32 pauseCount = *(UInt32*)((char*)ui + kUiPauseCountOffset);
        static std::atomic<bool> s_menuBlockLogged{false};
        if (pauseCount != 0)
        {
            if (!s_menuBlockLogged.exchange(true))
                logline("[f4-coop] blocked: menu/pause mode (pauseCount=%u)",
                        pauseCount);
            return;
        }
        s_menuBlockLogged = false;

        float cur[3];
        memcpy(cur, (char*)player + kPosOffset, sizeof(cur));

        // safety: never drag the player across the world (stale anchor,
        // corrupt packet)
        float ddx = g.x - cur[0], ddy = g.y - cur[1], ddz = g.z - cur[2];
        float dist2 = ddx * ddx + ddy * ddy + ddz * ddz;
        static std::atomic<bool> s_farLogged{false};
        if (dist2 > kMaxDragDist * kMaxDragDist)
        {
            if (!s_farLogged.exchange(true))
                logline("[f4-coop] blocked: ghost too far (%.0f units)",
                        sqrtf(dist2));
            return;
        }
        s_farLogged = false;

        _GetWorldspace getWorldspace =
            (_GetWorldspace)(g_gameBase + kGetWorldspaceOffset);
        void* worldspace = getWorldspace(player);

        // face the direction of movement (euler Z rotation, radians)
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
    // kMessage_GameDataReady also value-encodes its bool as the data
    // pointer (Hooks_GameData.cpp:29) - non-null check only, no deref.
    if (msg->type == F4SEMessagingInterface::kMessage_GameDataReady && msg->data)
        logline("[f4-coop] GameDataReady");
    if (msg->type == F4SEMessagingInterface::kMessage_GameLoaded)
    {
        logline("[f4-coop] GameLoaded");
        g_worldLoaded = true;
    }
    if (msg->type == F4SEMessagingInterface::kMessage_PreLoadGame)
    {
        logline("[f4-coop] PreLoadGame - apply disabled");
        g_applyArmed = false;
    }
    if (msg->type == F4SEMessagingInterface::kMessage_PostLoadGame)
    {
        // F4SE dispatches this with data = (void*)result - the bool is
        // value-encoded as the pointer itself (Hooks_SaveLoad.cpp:51),
        // NOT a pointer to a bool. Never dereference msg->data here
        // (v7 did and crashed the game on every save load).
        bool ok = (msg->data != nullptr);
        logline("[f4-coop] PostLoadGame ok=%d - apply %s (%u ms grace)",
                ok, ok ? "armed" : "disabled", kArmGraceMs);
        g_applyArmed = ok;
        if (ok)
            g_armedAtTick = GetTickCount64();
    }
    if (msg->type == F4SEMessagingInterface::kMessage_NewGame)
    {
        logline("[f4-coop] NewGame - apply disabled");
        g_applyArmed = false;
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
        bool armedNow =
            g_applyArmed &&
            (GetTickCount64() - g_armedAtTick.load() >= kArmGraceMs);
        static bool s_graceLogged = false;
        if (armedNow && !s_graceLogged)
        {
            s_graceLogged = true;
            logline("[f4-coop] grace elapsed, applying moves");
        }
        if (!g_applyArmed)
            s_graceLogged = false; // re-log after the next load
        if (armedNow && version != lastPosted && g_tasks &&
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
        info->version     = 9;

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
        logline("[f4-coop] loaded v9, handle=%u runtime=0x%08X",
                g_pluginHandle, f4se->runtimeVersion);
        return true;
    }
}
