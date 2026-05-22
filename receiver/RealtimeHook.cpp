#define _CRT_SECURE_NO_WARNINGS
#include "RealtimeHook.h"
#include "DelayManager.h"
#include "DiagnosticsLog.h"
#include "ServerStateSniffer.h"
#include "TickDelayBuffer.h"
#include <cstdarg>
#include <cstdio>

static FILE* g_hookLog = nullptr;
static volatile bool g_hookReady = false;

constexpr BYTE MSG_KILL = 0x50;
constexpr BYTE MSG_STATE_VAR = 0x5A;
constexpr BYTE MSG_PLAYER_STATUS = 0x17;

static void HookLog(const char* fmt, ...) {
    if (!DiagnosticsLog_IsEnabled()) return;

    if (!g_hookLog) {
        g_hookLog = fopen("killdetect_debug.log", "w");
        if (!g_hookLog) return;
    }

    va_list args;
    va_start(args, fmt);
    DiagnosticsLog_Write(g_hookLog, fmt, args);
    va_end(args);
}

void SetHookReady() {
    g_hookReady = true;
    HookLog("[RealtimeHook] Raw packet sniffing activated\n");
}

bool InitRealtimeHook(uintptr_t baseGame) {
    InitServerStateSniffer(baseGame);
    HookLog("[RealtimeHook] Initialized (ServerStateSniffer owns parsing)\n");
    return true;
}

void ShutdownRealtimeHook() {
    g_hookReady = false;
    ShutdownServerStateSniffer();
    if (g_hookLog) {
        fclose(g_hookLog);
        g_hookLog = nullptr;
    }
}

void ScanBufferForKills(BYTE* data, DWORD size) {
    if (!g_hookReady || !data || size == 0 || data[0] != MSG_KILL) return;
    ServerStateSniffer_OnRawMessage(data, size);
}

void ScanBufferForFlags(BYTE* data, DWORD size) {
    if (!g_hookReady || !data || size == 0 || data[0] != MSG_STATE_VAR) return;
    ServerStateSniffer_OnRawMessage(data, size);
}

void ScanBufferForPlayerPositions(BYTE* data, DWORD size) {
    if (!g_hookReady || !data || size == 0 || data[0] != MSG_PLAYER_STATUS) return;
    ServerStateSniffer_OnRawMessage(data, size);
}

bool RealtimeHook_GetPredictedPlayerPosition(int playerHandle, float outPos[3], DWORD maxAgeMs) {
    if (!outPos || playerHandle == 0) return false;

    int gameDelaySec = DelayManager::GetGameDelaySeconds();
    if (gameDelaySec < 0) gameDelaySec = 0;

    int localDelayMs = GetTickDelayMs();
    if (localDelayMs < 0) localDelayMs = 0;

    DWORD now = GetTickCount();
    DWORD visibleTick = now + (DWORD)(gameDelaySec * 1000 + localDelayMs);

    PlayerSnapshot snapshot = {};
    if (!ServerStateSniffer_GetSnapshotAt(playerHandle, visibleTick, &snapshot)) {
        return false;
    }

    if (maxAgeMs > 0 && now - snapshot.rawTick > maxAgeMs) {
        return false;
    }

    outPos[0] = snapshot.pos[0];
    outPos[1] = snapshot.pos[1];
    outPos[2] = snapshot.pos[2];
    return true;
}
