#define _CRT_SECURE_NO_WARNINGS
#include "RealtimeHook.h"
#include "DiagnosticsLog.h"
#include "SpectatorController.h"
#include "DelayManager.h"
#include "TickDelayBuffer.h"
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
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
    DiagnosticsLog_Write(g_killLog, fmt, args);
    va_end(args);
}

// ============================================================================
// Message type constants (first byte of message data)
// ============================================================================

constexpr BYTE MSG_KILL = 0x50;       // Kill score event (80 decimal)
constexpr int KILL_MSG_SIZE = 10;     // Minimum size of kill message
constexpr BYTE MSG_STATE_VAR = 0x5A;  // State variable update (90 decimal)
constexpr int STATE_VAR_MSG_SIZE = 9; // 1 opcode + 4 index + 4 value
constexpr BYTE MSG_PLAYER_STATUS = 0x17; // Live player status bitstream
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

struct PredictedPlayerPosition {
    float pos[3];
    DWORD timestamp;
    bool hasZ;
};

static std::unordered_map<int, PredictedPlayerPosition> g_predictedPositions;

struct StatusBitReader {
    const BYTE* data;
    DWORD size;
    DWORD bitOffset;

    bool ReadBits(unsigned int bitCount, unsigned int* out) {
        if (!out || bitCount > 32) return false;
        if (bitOffset + bitCount > size * 8) return false;

        unsigned int value = 0;
        for (unsigned int i = 0; i < bitCount; ++i) {
            DWORD srcBit = bitOffset + i;
            unsigned int bit = (data[srcBit >> 3] >> (srcBit & 7)) & 1u;
            value |= bit << i;
        }

        bitOffset += bitCount;
        *out = value;
        return true;
    }

    bool SkipBits(unsigned int bitCount) {
        if (bitOffset + bitCount > size * 8) return false;
        bitOffset += bitCount;
        return true;
    }
};

static float DecodeStatusFloat22(unsigned int value) {
    // Matches GNET_CLN_PL_UpdateStatus' compact float decoder.
    if (value == 35u) return 0.0f;

    unsigned int raw = value >> 1;
    int exponent = (int)(raw & 0x1Fu);
    if (exponent & 0x10) exponent -= 0x20;

    unsigned int bits = ((value & 1u) << 31)
                      | ((unsigned int)(exponent + 0x80) << 23)
                      | ((raw & 0x1FFFE0u) << 2);

    float out;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

static bool SkipStatusBits(StatusBitReader& reader, unsigned int flags) {
    if ((flags & 0x00001u) && !reader.SkipBits(2)) return false;
    if ((flags & 0x00002u) && !reader.SkipBits(3)) return false;
    if ((flags & 0x00004u) && !reader.SkipBits(10)) return false;
    if ((flags & 0x00008u) && !reader.SkipBits(8)) return false;
    if ((flags & 0x00010u) && !reader.SkipBits(8)) return false;
    if ((flags & 0x00020u) && !reader.SkipBits(9)) return false;
    if ((flags & 0x00040u) && !reader.SkipBits(8)) return false;
    if ((flags & 0x00080u) && !reader.SkipBits(8)) return false;
    return true;
}

static bool SkipStatusBitsAfterPosition(StatusBitReader& reader, unsigned int flags) {
    if ((flags & 0x00800u) && !reader.SkipBits(7)) return false;
    if ((flags & 0x01000u) && !reader.SkipBits(2)) return false;
    if ((flags & 0x00200u) && !reader.SkipBits(32)) return false;
    if ((flags & 0x00400u) && !reader.SkipBits(36)) return false;
    if ((flags & 0x02000u) && !reader.SkipBits(10)) return false;
    if ((flags & 0x08000u) && !reader.SkipBits(10)) return false;
    if ((flags & 0x10000u) && !reader.SkipBits(10)) return false;
    if ((flags & 0x04000u) && !reader.SkipBits(10)) return false;
    if ((flags & 0x20000u) && !reader.SkipBits(72)) return false;
    if ((flags & 0x40000u) && !reader.SkipBits(48)) return false;
    if (flags & 0x80000u) {
        unsigned int present = 0;
        if (!reader.ReadBits(1, &present)) return false;
        if (present && !reader.SkipBits(72)) return false;
    }
    return true;
}

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
    int leadMs = totalDelayMs - waitMs;
    if (leadMs < 0) leadMs = 0;
    int weaponId = (int)msgData[1];

    KillLog("[RealtimeHook] Kill detected: killer=%d victim=%d weapon=%d gameDelay=%ds bufferDelay=%dms scheduling in %dms\n",
            killerHandle, victimHandle, weaponId, gameDelaySec, bufferDelayMs, waitMs);

    auto action = DelayedAction::CreateKillAction(killerHandle, victimHandle, weaponId, waitMs, leadMs);
    DelayManager::AddDelayedAction(action);
}

// ============================================================================
// Public API
// ============================================================================

void SetHookReady() {
    g_hookReady = true;
    KillLog("[RealtimeHook] Kill scanning activated\n");
}

bool InitRealtimeHook(uintptr_t baseGame) {
    (void)baseGame;
    KillLog("[RealtimeHook] Initialized (scanning via TickDelayBuffer hook)\n");
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
    int leadMs = totalDelayMs - waitMs;
    if (leadMs < 0) leadMs = 0;

    KillLog("[RealtimeHook] Flag change: US=%d VC=%d gameDelay=%ds bufferDelay=%dms scheduling in %dms\n",
            newUS, newVC, gameDelaySec, bufferDelayMs, waitMs);

    auto action = DelayedAction::CreateFlagAction(newUS, newVC, waitMs, leadMs);
    DelayManager::AddDelayedAction(action);
}

// ============================================================================
// Live player position tracking
// ============================================================================

void ScanBufferForPlayerPositions(BYTE* data, DWORD size) {
    if (!g_hookReady || !data || size < 2) return;
    if (data[0] != MSG_PLAYER_STATUS) return;

    StatusBitReader reader = { data, size, 16 };
    unsigned int count = data[1];
    DWORD now = GetTickCount();

    std::lock_guard<std::mutex> lock(g_realtimeMutex);

    for (unsigned int i = 0; i < count; ++i) {
        unsigned int handle = 0;
        unsigned int statusTimeBits = 0;
        unsigned int flags = 0;

        if (!reader.ReadBits(13, &handle)) break;
        if (!reader.ReadBits(32, &statusTimeBits)) break;
        if (!reader.ReadBits(20, &flags)) break;

        if (!SkipStatusBits(reader, flags)) break;

        bool hasPosition = (flags & 0x00100u) != 0;
        if (hasPosition) {
            unsigned int xBits = 0;
            unsigned int yBits = 0;
            unsigned int zPresent = 0;
            unsigned int zBits = 0;

            if (!reader.ReadBits(22, &xBits)) break;
            if (!reader.ReadBits(22, &yBits)) break;
            if (!reader.ReadBits(1, &zPresent)) break;
            if (zPresent && !reader.ReadBits(22, &zBits)) break;

            PredictedPlayerPosition next = {};
            auto existing = g_predictedPositions.find((int)handle);
            next.pos[0] = DecodeStatusFloat22(xBits);
            next.pos[1] = DecodeStatusFloat22(yBits);
            next.hasZ = zPresent != 0;
            if (next.hasZ) {
                next.pos[2] = DecodeStatusFloat22(zBits);
            } else if (existing != g_predictedPositions.end() && existing->second.hasZ) {
                next.pos[2] = existing->second.pos[2];
                next.hasZ = true;
            }
            next.timestamp = now;

            if (handle > 0 && next.hasZ) {
                g_predictedPositions[(int)handle] = next;
            }
        }

        if (!SkipStatusBitsAfterPosition(reader, flags)) break;
    }
}

bool RealtimeHook_GetPredictedPlayerPosition(int playerHandle, float outPos[3], DWORD maxAgeMs) {
    if (!outPos || playerHandle == 0) return false;

    std::lock_guard<std::mutex> lock(g_realtimeMutex);
    auto it = g_predictedPositions.find(playerHandle);
    if (it == g_predictedPositions.end()) return false;

    DWORD age = GetTickCount() - it->second.timestamp;
    if (age > maxAgeMs) return false;

    outPos[0] = it->second.pos[0];
    outPos[1] = it->second.pos[1];
    outPos[2] = it->second.pos[2];
    return true;
}
