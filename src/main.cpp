// f4-coop - Stage 1, step 1: load into the game and log messages.
// Target: Fallout 4 1.10.163.0, F4SE 0.6.23.
// PluginAPI.h expects UInt32 to exist (F4SE defines it in its own build).
typedef unsigned int UInt32;
typedef unsigned long long UInt64;

#include "f4se/PluginAPI.h"
#include "f4se_common/f4se_version.h"

#include <cstdio>
#include <cstdarg>

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;

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
        logline("[f4-coop] GameLoaded");
}

extern "C"
{
    __declspec(dllexport)
    bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name        = "f4-coop";
        info->version     = 1;

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
        logline("[f4-coop] loaded, handle=%u runtime=0x%08X",
                g_pluginHandle, f4se->runtimeVersion);
        return true;
    }
}