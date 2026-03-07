#define _CRT_SECURE_NO_WARNINGS
#include "RealtimeHook.h"
#include "SpectatorController.h"
#include "DelayManager.h"
#include "TickDelayBuffer.h"
#include <iostream>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <cstdarg>
#include <cstdio>

static FILE* g_killLog = nullptr;

static void KillLog(const char* fmt, ...) {
    if (!g_killLog) {
        g_killLog = fopen("killdetect_debug.log", "w");
        if (!g_killLog) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_killLog, fmt, args);
    fflush(g_killLog);
    va_end(args);
}

// ============================================================================
// Message type constants (first byte of message data)
// ============================================================================

constexpr BYTE MSG_KILL = 0x50;       // Kill score event (80 decimal)
constexpr int KILL_MSG_SIZE = 10;     // Minimum size of kill message
constexpr BYTE MSG_STATE_VAR = 0x5A;  // State variable update (90 decimal)
constexpr int STATE_VAR_MSG_SIZE = 9; // 1 opcode + 4 index + 4 value
constexpr int FLAG_INDEX_US = 0x200;  // US team flag carrier index
constexpr int FLAG_INDEX_VC = 0x201;  // VC team flag carrier index

// ============================================================================
// Kill deduplication
// ============================================================================

static std::mutex g_realtimeMutex;
static std::unordered_set<uint64_t> g_recentKills;
static DWORD g_lastKillCleanup = 0;
constexpr DWORD KILL_DEDUP_WINDOW_MS = 30000;
constexpr DWORD KILL_CLEANUP_INTERVAL_MS = 10000;

static uint64_t MakeKillKey(int killerId, int victimId) {
    DWORD timeBucket = GetTickCount() / 10000;
    return ((uint64_t)killerId << 32) | ((uint64_t)victimId << 16) | (timeBucket & 0xFFFF);
}

// ============================================================================
// State
// ============================================================================

static volatile bool g_hookReady = false;

// ============================================================================
// Kill handler
// ============================================================================

static void ProcessKillMessage(BYTE* msgData) {
    // Kill message format (from GNET_ProcessKillEvent):
    //   Byte 0:    0x50 (message type)
    //   Byte 1:    weapon/cause ID
    //   Bytes 2-5: victim DP handle (DWORD)
    //   Bytes 6-9: killer DP handle (DWORD)
    int victimHandle = *(int*)(msgData + 2);
    int killerHandle = *(int*)(msgData + 6);

    if (killerHandle == 0 && victimHandle == 0) return;

    uint64_t key = MakeKillKey(killerHandle, victimHandle);
    {
        std::lock_guard<std::mutex> lock(g_realtimeMutex);

        DWORD now = GetTickCount();
        if (now - g_lastKillCleanup > KILL_CLEANUP_INTERVAL_MS) {
            g_recentKills.clear();
            g_lastKillCleanup = now;
        }

        if (g_recentKills.count(key)) {
            return;
        }
        g_recentKills.insert(key);
    }

    int gameDelaySec = DelayManager::GetGameDelaySeconds();
    int bufferDelayMs = GetTickDelayMs();
    int totalDelayMs = gameDelaySec * 1000 + bufferDelayMs;
    constexpr int ADVANCE_MS = 5000;
    int waitMs = (totalDelayMs > ADVANCE_MS)
        ? totalDelayMs - ADVANCE_MS
        : 0;

    KillLog("[RealtimeHook] Kill detected: killer=%d victim=%d weapon=%d gameDelay=%ds bufferDelay=%dms scheduling in %dms\n",
            killerHandle, victimHandle, (int)msgData[1], gameDelaySec, bufferDelayMs, waitMs);

    auto action = DelayedAction::CreateKillAction(killerHandle, victimHandle, waitMs);
    DelayManager::AddDelayedAction(action);
}

// ============================================================================
// Public API
// ============================================================================

void SetHookReady() {
    g_hookReady = true;
    std::cout << "[RealtimeHook] Kill scanning activated\n";
}

bool InitRealtimeHook(uintptr_t baseGame) {
    std::cout << "[RealtimeHook] Initialized (scanning via TickDelayBuffer hook)\n";
    return true;
}

void ShutdownRealtimeHook() {
    g_hookReady = false;
}

void ScanBufferForKills(BYTE* data, DWORD size) {
    if (!g_hookReady || !data || size < KILL_MSG_SIZE) return;
    if (data[0] != MSG_KILL) return;

    // Each NET_ReadMessages call returns exactly one message.
    // If first byte is 0x50, this IS the kill message (10 bytes).
    int victimHandle = *(int*)(data + 2);
    int killerHandle = *(int*)(data + 6);

    if (killerHandle > 0 && killerHandle < 0x10000 &&
        victimHandle > 0 && victimHandle < 0x10000) {
        ProcessKillMessage(data);
    }
}

// ============================================================================
// Flag carrier tracking
// ============================================================================

static int g_lastRawUSCarrier = 0;
static int g_lastRawVCCarrier = 0;

void ScanBufferForFlags(BYTE* data, DWORD size) {
    if (!g_hookReady || !data || size < STATE_VAR_MSG_SIZE) return;
    if (data[0] != MSG_STATE_VAR) return;

    // State variable update format (opcode 0x5A, 9 bytes):
    //   byte[0]   = 0x5A
    //   bytes[1-4] = index (DWORD LE) — 0x200 = US flag, 0x201 = VC flag
    //   bytes[5-8] = value (DWORD LE) — player handle or 0
    int index = *(int*)(data + 1);
    int value = *(int*)(data + 5);

    if (index != FLAG_INDEX_US && index != FLAG_INDEX_VC) return;

    // Track both carriers, only fire action on change
    int newUS = g_lastRawUSCarrier;
    int newVC = g_lastRawVCCarrier;

    if (index == FLAG_INDEX_US) newUS = value;
    if (index == FLAG_INDEX_VC) newVC = value;

    if (newUS == g_lastRawUSCarrier && newVC == g_lastRawVCCarrier) return;

    g_lastRawUSCarrier = newUS;
    g_lastRawVCCarrier = newVC;

    int gameDelaySec = DelayManager::GetGameDelaySeconds();
    int bufferDelayMs = GetTickDelayMs();
    int totalDelayMs = gameDelaySec * 1000 + bufferDelayMs;
    constexpr int ADVANCE_MS = 5000;
    int waitMs = (totalDelayMs > ADVANCE_MS)
        ? totalDelayMs - ADVANCE_MS
        : 0;

    KillLog("[RealtimeHook] Flag change: US=%d VC=%d gameDelay=%ds bufferDelay=%dms scheduling in %dms\n",
            newUS, newVC, gameDelaySec, bufferDelayMs, waitMs);

    auto action = DelayedAction::CreateFlagAction(newUS, newVC, waitMs);
    DelayManager::AddDelayedAction(action);
}
