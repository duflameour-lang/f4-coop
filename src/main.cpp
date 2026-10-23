// f4-coop - F4SE plugin: co-op presence for Fallout 4 1.10.163 (pre-NG).
// v15: net = f4mp mode - the plugin is an F4Multiplayer protocol client
// (vendored common/Protocol.h + ENet): connects to an F4MPServer, sends
// its own PlayerState at 10 Hz, and drives one puppet per remote player
// from kPlayerStates relays. Legacy net = our raw-UDP relay stays as a
// fallback (net = legacy).
// History: v6 MoveRefrToPosition; v7 msg-data bool crash fix; v8/v9
// load-lifecycle gates + grace; v10 puppet-not-player; v11 playerId;
// v12 host relay; v13/v14 death-check lesson (field is flags, not state).
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

// F4MP protocol client (vendored, GPL-3+exceptions - see README)
#include "Protocol.h"
#include "Net.h"

#include <enet/enet.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

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
static const uintptr_t kRotOffset            = 0xC0;       // TESObjectREFR::rot (euler, radians)
static const uintptr_t kPosOffset            = 0xD0;       // TESObjectREFR::pos
static const uintptr_t kFormIDOffset         = 0x14;       // TESForm::formID
static const uintptr_t kBaseFormOffset       = 0xE0;       // TESObjectREFR::baseForm
static const uintptr_t kInvalidHandleOffset  = 0x038CCE04; // g_invalidRefHandle
static const uintptr_t kMoveRefrOffset      = 0x013FE7E0; // MoveRefrToPosition
static const uintptr_t kGetWorldspaceOffset = 0x0040F290; // TESObjectREFR::GetWorldspace
static const uintptr_t kGameVMOffset         = 0x058D3308; // g_gameVM
static const uintptr_t kVMOffset             = 0xB0;       // GameVM::m_virtualMachine
static const uintptr_t kPlaceAtMeOffset      = 0x0140B0E0; // PlaceAtMe_Native
static const uintptr_t kUiSingletonOffset   = 0x058D0898; // g_ui
static const uintptr_t kUiPauseCountOffset  = 0x1E0;      // UI::numPauseGame

// NOT a life state (v13 lesson): flags-like field at Actor+0x130;
// 0x80000000 for living actors. Logged at spawn for future RE.
static const uintptr_t kLifeStateOffset = 0x130;

// safety: never drag the player farther than this in one move (units)
static const float kMaxDragDist = 5000.0f;

// after PostLoadGame, wait this long before applying any move: the load
// must finish undisturbed or the loading screen never dismisses
static const unsigned kArmGraceMs = 10000;

typedef void (*_MoveRefrToPosition)(void* source, UInt32* pTargetHandle,
    void* parentCell, void* worldSpace, float* position, float* rotation);
typedef void* (*_GetWorldspace)(void* refr);
typedef void* (*_PlaceAtMe_Native)(void* vm, UInt32 stackId, void** target,
    void* form, int count, bool forcePersist, bool initiallyDisabled,
    bool deleteWhenAble);

static unsigned int formIDOf(const void* form)
{
    return form ? *(const unsigned int*)((const char*)form + kFormIDOffset) : 0;
}

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

// ---- config (f4coop.ini in the game root) -------------------------------
struct Config
{
    bool isF4mp = true;             // net = f4mp | legacy
    // f4mp mode:
    char server[64] = "127.0.0.1:7779";
    char name[32] = "Player";
    // legacy mode:
    bool isHost = false;
    unsigned int myId = 1;
    unsigned short port = 31337;
    char hostAddr[64] = "127.0.0.1";
};
static Config g_cfg;

static void loadConfig()
{
    FILE* f = nullptr;
    fopen_s(&f, "f4coop.ini", "r");
    if (!f)
    {
        logline("[f4-coop] no f4coop.ini, defaults: net=f4mp -> %s",
                g_cfg.server);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[32] = "", value[64] = "";
        if (sscanf_s(line, " %31[^= \t\r\n] = %63s", key, 32, value, 64) != 2)
            continue;
        if (strcmp(key, "net") == 0)
            g_cfg.isF4mp = (strcmp(value, "f4mp") == 0);
        else if (strcmp(key, "server") == 0)
            strncpy_s(g_cfg.server, value, _TRUNCATE);
        else if (strcmp(key, "name") == 0)
            strncpy_s(g_cfg.name, value, _TRUNCATE);
        else if (strcmp(key, "role") == 0)
            g_cfg.isHost = (strcmp(value, "host") == 0);
        else if (strcmp(key, "id") == 0)
            g_cfg.myId = (unsigned int)strtoul(value, nullptr, 10);
        else if (strcmp(key, "host") == 0)
            strncpy_s(g_cfg.hostAddr, value, _TRUNCATE);
        else if (strcmp(key, "port") == 0)
        {
            unsigned long p = strtoul(value, nullptr, 10);
            if (p > 0 && p < 65536) g_cfg.port = (unsigned short)p;
        }
    }
    fclose(f);
    if (g_cfg.isF4mp)
        logline("[f4-coop] config: net=f4mp server=%s name=%s",
                g_cfg.server, g_cfg.name);
    else
        logline("[f4-coop] config: net=legacy role=%s id=%u -> %s:%u",
                g_cfg.isHost ? "host (listens)" : "client", g_cfg.myId,
                g_cfg.isHost ? "any" : g_cfg.hostAddr, g_cfg.port);
}

// ---- ghost state: net thread writes, game thread applies ----------------
struct GhostEntry
{
    float x, y, z;
    float heading; // radians, as broadcast by the remote player
};
static std::mutex g_ghostLock;
static std::map<unsigned int, GhostEntry> g_ghosts; // playerId -> pos
static UInt64 g_ghostVersion = 0;                  // bumped on every store
static std::atomic<bool> g_taskInFlight{false};

// one driven NPC per remote playerId; game-thread only (Run + load events)
static std::map<unsigned int, void*> g_puppets;


// ---- legacy UDP transport (net = legacy fallback) ------------------------
static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_dest = {};
static unsigned int g_seq = 0;

struct PosPacket
{
    unsigned int magic;
    unsigned int seq;
    unsigned int playerId; // 0 = the sending client's own player
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

    sockaddr_in self = {};
    self.sin_family = AF_INET;
    self.sin_port = g_cfg.isHost ? htons(g_cfg.port) : 0;
    self.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (sockaddr*)&self, sizeof(self)) != 0)
    {
        logline("[f4-coop] bind port %u FAILED (wsa=%d)",
                g_cfg.port, WSAGetLastError());
        return false;
    }

    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);

    if (!g_cfg.isHost)
    {
        g_dest.sin_family = AF_INET;
        g_dest.sin_port = htons(g_cfg.port);
        inet_pton(AF_INET, g_cfg.hostAddr, &g_dest.sin_addr);
    }
    return true;
}

static void sendTo(const sockaddr_in& to, unsigned int id, float x, float y, float z)
{
    if (g_sock == INVALID_SOCKET) return;
    PosPacket p;
    p.magic = kMagic;
    p.seq = g_seq++;
    p.playerId = id;
    p.x = x; p.y = y; p.z = z;
    sendto(g_sock, (const char*)&p, sizeof(p), 0,
           (const sockaddr*)&to, sizeof(to));
}

// record a remote player's position for the game thread to puppet
static void storeGhost(const PosPacket& p)
{
    if (p.playerId == g_cfg.myId) return; // never puppet self
    std::lock_guard<std::mutex> lk(g_ghostLock);
    g_ghostVersion++;
    g_ghosts[p.playerId] = { p.x, p.y, p.z, 0.0f };
}

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
        if (p.playerId == g_cfg.myId) continue;

        if (p.x != s_lastRxX)
        {
            logline("[f4-coop] rx id=%u seq=%u pos %.1f %.1f %.1f",
                    p.playerId, p.seq, p.x, p.y, p.z);
            s_lastRxX = p.x;
        }

        storeGhost(p);
    }
}

struct LegacyPeer
{
    sockaddr_in addr;
    unsigned int id;
    unsigned long long lastSeen;
};
static std::vector<LegacyPeer> g_peers; // net thread only

static void hostService(bool havePos, float x, float y, float z)
{
    static float s_lastRxX = 1e30f;
    for (int i = 0; i < 32; i++)
    {
        PosPacket p = {};
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        int n = recvfrom(g_sock, (char*)&p, sizeof(p), 0,
                         (sockaddr*)&from, &fromLen);
        if (n != sizeof(p)) break; // nothing more (WSAEWOULDBLOCK)
        if (p.magic != kMagic || p.playerId == g_cfg.myId) continue;

        bool known = false;
        for (auto& pr : g_peers)
        {
            if (pr.addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                pr.addr.sin_port == from.sin_port)
            {
                known = true;
                pr.lastSeen = GetTickCount64();
                pr.id = p.playerId;
                break;
            }
        }
        if (!known)
        {
            LegacyPeer np = { from, p.playerId, GetTickCount64() };
            g_peers.push_back(np);
            logline("[f4-coop] peer joined: id=%u", p.playerId);
        }

        if (p.x != s_lastRxX)
        {
            logline("[f4-coop] rx id=%u seq=%u pos %.1f %.1f %.1f",
                    p.playerId, p.seq, p.x, p.y, p.z);
            s_lastRxX = p.x;
        }

        storeGhost(p);
    }

    unsigned long long now = GetTickCount64();
    for (size_t i = 0; i < g_peers.size(); )
    {
        if (now - g_peers[i].lastSeen > 5000)
        {
            logline("[f4-coop] peer left: id=%u", g_peers[i].id);
            g_peers.erase(g_peers.begin() + i);
        }
        else ++i;
    }

    if (!havePos || g_peers.empty()) return;

    std::vector<std::pair<unsigned int, GhostEntry>> snap;
    snap.push_back(std::make_pair(g_cfg.myId, GhostEntry{ x, y, z, 0.0f }));
    {
        std::lock_guard<std::mutex> lk(g_ghostLock);
        for (auto& kv : g_ghosts)
            snap.push_back(kv);
    }
    for (auto& pr : g_peers)
        for (auto& s : snap)
            sendTo(pr.addr, s.first, s.second.x, s.second.y, s.second.z);
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

        std::vector<std::pair<unsigned int, GhostEntry>> ghosts;
        {
            std::lock_guard<std::mutex> lk(g_ghostLock);
            for (auto& kv : g_ghosts)
                ghosts.push_back(kv);
        }
        if (ghosts.empty()) return;

        void* player = *(void**)(g_gameBase + kPlayerPtrOffset);
        if (!player) return;

        // never move while a menu/loading screen is up (UI::numPauseGame,
        // "isInMenuMode" per GameMenus.h). Moving during a load screen
        // restarts cell streaming and hangs the load forever (v6/v8).
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

        for (auto& gh : ghosts)
        {
            void* puppet = nullptr;
            auto it = g_puppets.find(gh.first);
            if (it != g_puppets.end())
                puppet = it->second;

            // one-time puppet spawn per player: PlaceAtMe the player's own
            // base form, so every puppet is a copy of the player - no
            // FormID guesswork.
            if (!puppet)
            {
                void* baseForm = *(void**)((char*)player + kBaseFormOffset);
                void* gameVM = *(void**)(g_gameBase + kGameVMOffset);
                void* vm = gameVM ? *(void**)((char*)gameVM + kVMOffset) : nullptr;
                static std::atomic<int> s_spawnFails{0};
                if (!baseForm || !vm)
                {
                    if (++s_spawnFails <= 3)
                        logline("[f4-coop] spawn blocked: base=%p vm=%p",
                                baseForm, vm);
                    return;
                }
                _PlaceAtMe_Native placeAtMe =
                    (_PlaceAtMe_Native)(g_gameBase + kPlaceAtMeOffset);
                logline("[f4-coop] spawning puppet %u: player=%p base=%p vm=%p",
                        gh.first, player, baseForm, vm);
                puppet = placeAtMe(vm, 0, &player, baseForm, 1, false, false, false);
                if (!puppet)
                {
                    if (++s_spawnFails <= 3)
                        logline("[f4-coop] PlaceAtMe(%u) returned null", gh.first);
                    return;
                }
                s_spawnFails = 0;
                g_puppets[gh.first] = puppet;
                logline("[f4-coop] puppet %u spawned: %p lifeState=%u",
                        gh.first, puppet,
                        *(UInt32*)((char*)puppet + kLifeStateOffset));
            }

            void* parentCell = *(void**)((char*)puppet + kParentCellOffset);
            if (!parentCell) continue;

            _GetWorldspace getWorldspace =
                (_GetWorldspace)(g_gameBase + kGetWorldspaceOffset);
            void* worldspace = getWorldspace(puppet);

            float cur[3];
            memcpy(cur, (char*)puppet + kPosOffset, sizeof(cur));

            // safety: never drag a puppet across the world (stale anchor,
            // corrupt packet)
            float ddx = gh.second.x - cur[0], ddy = gh.second.y - cur[1],
                  ddz = gh.second.z - cur[2];
            float dist2 = ddx * ddx + ddy * ddy + ddz * ddz;
            static std::atomic<bool> s_farLogged{false};
            if (dist2 > kMaxDragDist * kMaxDragDist)
            {
                if (!s_farLogged.exchange(true))
                    logline("[f4-coop] blocked: ghost %u too far (%.0f units)",
                            gh.first, sqrtf(dist2));
                continue;
            }
            s_farLogged = false;

            // face where the remote player faces (euler Z rotation, radians;
            // fall back to the direction of movement when their heading is 0)
            float yaw = gh.second.heading;
            if (yaw == 0.0f && (ddx != 0.0f || ddy != 0.0f))
                yaw = atan2f(ddy, ddx);
            float rot[3] = { 0.f, 0.f, yaw };
            float pos[3] = { gh.second.x, gh.second.y, gh.second.z };

            UInt32 nullHandle = *(UInt32*)(g_gameBase + kInvalidHandleOffset);
            _MoveRefrToPosition moveRefr =
                (_MoveRefrToPosition)(g_gameBase + kMoveRefrOffset);

            static std::atomic<UInt64> s_moves{0};
            UInt64 n = ++s_moves;
            bool logIt = (n <= 5 || n % 50 == 0);
            if (logIt)
                logline("[f4-coop] moveRefr #%llu id=%u -> %.1f %.1f %.1f (cell=%p ws=%p)",
                        (unsigned long long)n, gh.first, gh.second.x, gh.second.y,
                        gh.second.z, parentCell, worldspace);

            moveRefr(puppet, &nullHandle, parentCell, worldspace, pos, rot);

            if (logIt)
            {
                float after[3];
                memcpy(after, (char*)puppet + kPosOffset, sizeof(after));
                logline("[f4-coop] move #%llu id=%u done, pos now %.1f %.1f %.1f",
                        (unsigned long long)n, gh.first, after[0], after[1], after[2]);
            }
        }
    }
};

// ---- F4MP protocol client -----------------------------------------------
struct F4mpClient
{
    std::atomic<bool> welcomed{false};
    std::atomic<unsigned int> myPlayerId{0};
};
static F4mpClient g_f4mp;

static void f4mpHandlePacket(std::span<const std::uint8_t> a_data)
{
    using namespace Protocol;
    switch (PeekType(a_data).value_or(MessageType{}))
    {
    case MessageType::kWelcome:
        if (const auto msg = DecodeWelcome(a_data))
        {
            g_f4mp.myPlayerId = msg->playerId;
            g_f4mp.welcomed = true;
            logline("[f4-coop] welcomed as player %u in session %016llx",
                    msg->playerId, (unsigned long long)msg->sessionId);
        }
        break;
    case MessageType::kReject:
        if (const auto msg = DecodeReject(a_data))
            logline("[f4-coop] REJECTED: %s", msg->reason.c_str());
        break;
    case MessageType::kPlayerJoined:
        if (const auto msg = DecodePlayerJoined(a_data))
            logline("[f4-coop] player %u joined: %s",
                    msg->playerId, msg->name.c_str());
        break;
    case MessageType::kPlayerLeft:
        if (const auto msg = DecodePlayerLeft(a_data))
            logline("[f4-coop] player %u left", msg->playerId);
        break;
    case MessageType::kRemoved:
        if (const auto msg = DecodeRemoved(a_data))
            logline("[f4-coop] removed by host: %s",
                    msg->reason == RemovedReason::kKicked ? "kicked" : "banned");
        break;
    case MessageType::kPlayerStates:
        if (const auto msg = DecodePlayerStates(a_data))
        {
            for (const auto& rs : msg->players)
            {
                if (rs.playerId == g_f4mp.myPlayerId.load()) continue;
                std::lock_guard<std::mutex> lk(g_ghostLock);
                g_ghostVersion++;
                g_ghosts[rs.playerId] = { rs.state.x, rs.state.y, rs.state.z,
                                          rs.state.heading };
            }
        }
        break;
    default:
        break;
    }
}

static void f4mpThreadMain()
{
    using Clock = std::chrono::steady_clock;
    if (!Net::Initialize())
    {
        logline("[f4-coop] enet init failed");
        return;
    }
    ENetAddress address{};
    if (!Net::ResolveAddress(g_cfg.server, Protocol::DEFAULT_PORT, address))
    {
        logline("[f4-coop] cannot resolve server %s", g_cfg.server);
        return;
    }
    ENetHost* host = enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0);
    if (!host)
    {
        logline("[f4-coop] enet host create failed");
        return;
    }
    ENetPeer* peer = enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0);
    if (!peer)
    {
        logline("[f4-coop] enet connect failed");
        return;
    }
    logline("[f4-coop] f4mp client connecting to %s ...", g_cfg.server);

    _GetWorldspace getWorldspace =
        (_GetWorldspace)(g_gameBase + kGetWorldspaceOffset);

    UInt32 sequence = 0;
    UInt64 lastPosted = 0;
    float lastX = 0, lastY = 0, lastZ = 0;
    bool haveLast = false;
    Clock::time_point lastPosAt{};
    Clock::time_point nextSend{};
    Clock::time_point nextConnect{};

    for (;;)
    {
        const auto now = Clock::now();

        if (!peer && now >= nextConnect)
        {
            peer = enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0);
            nextConnect = now + std::chrono::seconds(3);
        }

        // service the connection (up to 100 ms) - this is our loop tick
        ENetEvent event;
        while (peer && enet_host_service(host, &event, 100) > 0)
        {
            switch (event.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                logline("[f4-coop] connected, sending hello");
                Net::Send(peer,
                    Protocol::Encode(Protocol::Hello{
                        .name = g_cfg.name,
                    }),
                    true);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                f4mpHandlePacket(
                    std::span<const std::uint8_t>{ event.packet->data,
                                                   event.packet->dataLength });
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                logline("[f4-coop] disconnected");
                g_f4mp.welcomed = false;
                peer = nullptr;
                nextConnect = now + std::chrono::seconds(3);
                break;
            default:
                break;
            }
        }

        // send our own state at 10 Hz
        if (g_f4mp.welcomed && g_worldLoaded && now >= nextSend)
        {
            nextSend = now + std::chrono::milliseconds(100);
            void* player = *(void**)(g_gameBase + kPlayerPtrOffset);
            if (player)
            {
                float x = *(float*)((char*)player + kPosOffset);
                float y = *(float*)((char*)player + kPosOffset + 4);
                float z = *(float*)((char*)player + kPosOffset + 8);

                float speed = 0.0f;
                if (haveLast)
                {
                    const float dt = std::chrono::duration<float>(now - lastPosAt).count();
                    const float dx = x - lastX, dy = y - lastY, dz = z - lastZ;
                    if (dt > 0.001f)
                        speed = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
                }
                lastX = x; lastY = y; lastZ = z; lastPosAt = now;
                haveLast = true;

                void* parentCell = *(void**)((char*)player + kParentCellOffset);
                void* worldspace = getWorldspace(player);

                Protocol::PlayerState s{};
                s.sequence = ++sequence;
                s.cell = worldspace ? 0 : formIDOf(parentCell);
                s.worldspace = formIDOf(worldspace);
                s.x = x; s.y = y; s.z = z;
                s.heading = *(float*)((char*)player + kRotOffset + 8); // rot.z
                s.speed = speed;
                // 0x01 forward, 0x40 walking, 0x80 running (ActorState::moveMode)
                s.moveMode = speed > 200.0f ? std::uint16_t{ 0x81 }
                          : speed > 1.0f   ? std::uint16_t{ 0x41 }
                                           : std::uint16_t{ 0 };
                s.flags = speed > 200.0f ? Protocol::kRunning : Protocol::kNone;
                s.sentMs = Protocol::SentMs(now);
                Net::Send(peer, Protocol::Encode(s), false);
            }
        }

        // post at most one apply task at a time, and only on fresh data
        UInt64 version;
        {
            std::lock_guard<std::mutex> lk(g_ghostLock);
            version = g_ghostVersion;
        }
        bool armedNow =
            g_applyArmed && g_f4mp.welcomed &&
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
    }
}
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
        logline("[f4-coop] PreLoadGame - apply disabled, puppets cleared");
        g_applyArmed = false;
        g_puppets.clear(); // stale after the world changes
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
        logline("[f4-coop] NewGame - apply disabled, puppets cleared");
        g_applyArmed = false;
        g_puppets.clear();
    }
}

static void legacyThreadMain()
{
    float lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
    bool haveLast = false;
    UInt64 lastPosted = 0;
    for (;;)
    {
        Sleep(100);

        float x = 0.f, y = 0.f, z = 0.f;
        bool havePos = false;
        if (g_worldLoaded)
        {
            void* player = *(void**)(g_gameBase + kPlayerPtrOffset);
            if (player)
            {
                x = *(float*)((char*)player + kPosOffset);
                y = *(float*)((char*)player + kPosOffset + 4);
                z = *(float*)((char*)player + kPosOffset + 8);
                havePos = true;
            }
        }

        if (g_cfg.isHost)
            hostService(havePos, x, y, z);
        else if (havePos)
        {
            sendTo(g_dest, g_cfg.myId, x, y, z);
            udpTryRecv();
        }

        UInt64 version;
        {
            std::lock_guard<std::mutex> lk(g_ghostLock);
            version = g_ghostVersion;
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

        if (havePos)
        {
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
}

extern "C"
{
    __declspec(dllexport)
    bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name        = "f4-coop";
        info->version     = 15;

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

        loadConfig();

        std::thread t(g_cfg.isF4mp ? f4mpThreadMain : legacyThreadMain);
        t.detach();
        logline("[f4-coop] loaded v15 (net=%s), handle=%u runtime=0x%08X",
                g_cfg.isF4mp ? "f4mp" : "legacy", g_pluginHandle,
                f4se->runtimeVersion);
        return true;
    }
}
