#define _CRT_SECURE_NO_WARNINGS
#include "ServerStateSniffer.h"
#include "DelayManager.h"
#include "DiagnosticsLog.h"
#include "ServerTelemetry.h"
#include "TickDelayBuffer.h"
#include <cstring>
#include <mutex>
#include <unordered_set>

// ============================================================================
// Message type constants
// ============================================================================

constexpr BYTE MSG_KILL = 0x50;
constexpr int KILL_MSG_SIZE = 10;
constexpr BYTE MSG_STATE_VAR = 0x5A;
constexpr int STATE_VAR_MSG_SIZE = 9;
constexpr BYTE MSG_PLAYER_STATUS = 0x17;
constexpr int FLAG_INDEX_US = 0x200;
constexpr int FLAG_INDEX_VC = 0x201;

// ============================================================================
// State
// ============================================================================

static std::mutex g_snifferMutex;
static uintptr_t g_gameBase = 0;
static unsigned long long g_nextSequence = 0;

static std::vector<DirectorEvent> g_events;
static const size_t MAX_PENDING_EVENTS = 256;

static const int SNAPSHOT_RING_SIZE = 2048;
static PlayerSnapshot g_snapshotRing[SNAPSHOT_RING_SIZE];
static int g_snapshotHead = 0;
static int g_snapshotCount = 0;

static std::unordered_set<unsigned long long> g_recentKills;
static DWORD g_lastKillCleanup = 0;
constexpr DWORD KILL_CLEANUP_INTERVAL_MS = 10000;

static int g_lastRawUSCarrier = 0;
static int g_lastRawVCCarrier = 0;

static void ApplyFlagTelemetry(DirectorEvent& event, const ServerTelemetryFlagEvent& telemetry) {
    event.hasFlagTelemetry = true;
    event.flagAction = telemetry.action;
    event.flagSide = telemetry.flagSide;
    event.flagReasonFlags = telemetry.flags;
    event.flagCarryTimeMs = telemetry.carryTimeMs;
    event.flagCarryDistanceMeters = telemetry.carryDistanceMeters;
    event.flagPlayerHandle = telemetry.playerId;
    event.flagPlayerTeam = telemetry.playerTeam;
    strncpy_s(event.flagPlayerName, telemetry.playerName, _TRUNCATE);
}

static unsigned int InferFlagAction(const DirectorEvent& event, int* playerHandle) {
    if (playerHandle) *playerHandle = 0;

    if (event.previousUSCarrier == 0 && event.usCarrier != 0) {
        if (playerHandle) *playerHandle = event.usCarrier;
        return 1;
    }
    if (event.previousVCCarrier == 0 && event.vcCarrier != 0) {
        if (playerHandle) *playerHandle = event.vcCarrier;
        return 1;
    }
    if (event.previousUSCarrier != 0 && event.usCarrier == 0) {
        if (playerHandle) *playerHandle = event.previousUSCarrier;
        return 2;
    }
    if (event.previousVCCarrier != 0 && event.vcCarrier == 0) {
        if (playerHandle) *playerHandle = event.previousVCCarrier;
        return 2;
    }
    return 0;
}

static void EnrichFlagEvent(DirectorEvent& event) {
    int playerHandle = 0;
    unsigned int action = InferFlagAction(event, &playerHandle);

    ServerTelemetryFlagEvent telemetry = {};
    if (ServerTelemetry_TryFindFlag(playerHandle, action, event.rawTick, &telemetry) ||
        ServerTelemetry_TryFindFlag(0, action, event.rawTick, &telemetry) ||
        ServerTelemetry_TryFindFlag(0, 0, event.rawTick, &telemetry)) {
        ApplyFlagTelemetry(event, telemetry);
    } else {
        event.flagAction = action;
        event.flagPlayerHandle = playerHandle;
    }
}

// ============================================================================
// Logging
// ============================================================================

static const char* EventTypeName(DirectorEventType type) {
    return type == DirectorEventType::Flag ? "flag" : "kill";
}

static void TimingLogEvent(const DirectorEvent& event, const char* reason) {
    DiagnosticsLog_Append(
        "camera_timing_debug.log",
        "[Event] seq=%llu type=%s raw=%lu playbackDelay=%d visible=%lu lead=%d killer=%d victim=%d weapon=%d us=%d vc=%d reason=%s\n",
        event.sequence,
        EventTypeName(event.type),
        event.rawTick,
        event.playbackDelayMs,
        event.visibleTick,
        event.availableLeadMs,
        event.killerHandle,
        event.victimHandle,
        event.weaponId,
        event.usCarrier,
        event.vcCarrier,
        reason ? reason : "none");
}

// ============================================================================
// Timing
// ============================================================================

static int GetPlaybackDelayMs() {
    int gameDelaySec = DelayManager::GetGameDelaySeconds();
    if (gameDelaySec < 0) gameDelaySec = 0;

    int localDelayMs = GetTickDelayMs();
    if (localDelayMs < 0) localDelayMs = 0;

    return gameDelaySec * 1000 + localDelayMs;
}

static void FillEventTiming(DirectorEvent& event, DWORD rawTick) {
    int playbackDelayMs = GetPlaybackDelayMs();
    event.rawTick = rawTick;
    event.playbackDelayMs = playbackDelayMs;
    event.availableLeadMs = playbackDelayMs;
    event.visibleTick = rawTick + (DWORD)playbackDelayMs;
}

// ============================================================================
// Kill deduplication
// ============================================================================

static unsigned long long MakeKillKey(int killerId, int victimId, DWORD now) {
    DWORD timeBucket = now / 10000;
    return ((unsigned long long)killerId << 32)
         | ((unsigned long long)victimId << 16)
         | (timeBucket & 0xFFFF);
}

static bool RememberKill(int killerHandle, int victimHandle, DWORD now) {
    if (now - g_lastKillCleanup > KILL_CLEANUP_INTERVAL_MS) {
        g_recentKills.clear();
        g_lastKillCleanup = now;
    }

    unsigned long long key = MakeKillKey(killerHandle, victimHandle, now);
    if (g_recentKills.count(key)) {
        return false;
    }

    g_recentKills.insert(key);
    return true;
}

// ============================================================================
// Snapshot ring
// ============================================================================

static void StoreSnapshotLocked(const PlayerSnapshot& snapshot) {
    g_snapshotRing[g_snapshotHead] = snapshot;
    g_snapshotHead = (g_snapshotHead + 1) % SNAPSHOT_RING_SIZE;
    if (g_snapshotCount < SNAPSHOT_RING_SIZE) {
        ++g_snapshotCount;
    }
}

static bool GetSnapshotAtLocked(int handle, DWORD visibleTick, PlayerSnapshot* out) {
    if (!out || handle == 0) return false;

    bool found = false;
    PlayerSnapshot best = {};
    DWORD bestDiff = 0xFFFFFFFF;

    for (int i = 0; i < g_snapshotCount; ++i) {
        const PlayerSnapshot& snap = g_snapshotRing[i];
        if (!snap.hasPosition || snap.handle != handle) continue;

        DWORD diff = snap.visibleTick > visibleTick
            ? (snap.visibleTick - visibleTick)
            : (visibleTick - snap.visibleTick);

        if (!found || diff < bestDiff) {
            found = true;
            best = snap;
            bestDiff = diff;
        }
    }

    // Status packets are frequent. A wider window keeps killcam planning useful
    // through packet jitter without accepting very old positions.
    if (!found || bestDiff > 10000) {
        return false;
    }

    *out = best;
    return true;
}

// ============================================================================
// Status bitstream parsing
// ============================================================================

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

static bool FindPreviousSnapshotZLocked(int handle, float* outZ) {
    if (!outZ) return false;

    for (int offset = 1; offset <= g_snapshotCount; ++offset) {
        int idx = g_snapshotHead - offset;
        if (idx < 0) idx += SNAPSHOT_RING_SIZE;

        const PlayerSnapshot& snap = g_snapshotRing[idx];
        if (snap.hasPosition && snap.handle == handle) {
            *outZ = snap.pos[2];
            return true;
        }
    }

    return false;
}

static void ProcessPlayerStatus(BYTE* data, DWORD size, DWORD rawTick) {
    if (size < 2) return;

    StatusBitReader reader = { data, size, 16 };
    unsigned int count = data[1];
    int playbackDelayMs = GetPlaybackDelayMs();

    for (unsigned int i = 0; i < count; ++i) {
        unsigned int handle = 0;
        unsigned int statusTimeBits = 0;
        unsigned int flags = 0;

        if (!reader.ReadBits(13, &handle)) break;
        if (!reader.ReadBits(32, &statusTimeBits)) break;
        if (!reader.ReadBits(20, &flags)) break;
        (void)statusTimeBits;

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

            PlayerSnapshot snapshot = {};
            snapshot.handle = (int)handle;
            snapshot.rawTick = rawTick;
            snapshot.visibleTick = rawTick + (DWORD)playbackDelayMs;
            snapshot.pos[0] = DecodeStatusFloat22(xBits);
            snapshot.pos[1] = DecodeStatusFloat22(yBits);
            snapshot.hasPosition = true;

            if (zPresent) {
                snapshot.pos[2] = DecodeStatusFloat22(zBits);
            } else if (!FindPreviousSnapshotZLocked(snapshot.handle, &snapshot.pos[2])) {
                snapshot.hasPosition = false;
            }

            if (snapshot.handle > 0 && snapshot.hasPosition) {
                StoreSnapshotLocked(snapshot);
            }
        }

        if (!SkipStatusBitsAfterPosition(reader, flags)) break;
    }
}

// ============================================================================
// Event parsing
// ============================================================================

static void PushEventLocked(DirectorEvent& event) {
    event.sequence = ++g_nextSequence;

    if (event.type == DirectorEventType::Kill) {
        event.hasKillerSnapshot = GetSnapshotAtLocked(
            event.killerHandle,
            event.visibleTick,
            &event.killerSnapshot);
        event.hasVictimSnapshot = GetSnapshotAtLocked(
            event.victimHandle,
            event.visibleTick,
            &event.victimSnapshot);
    } else {
        event.hasUSCarrierSnapshot = GetSnapshotAtLocked(
            event.usCarrier,
            event.visibleTick,
            &event.usCarrierSnapshot);
        event.hasVCCarrierSnapshot = GetSnapshotAtLocked(
            event.vcCarrier,
            event.visibleTick,
            &event.vcCarrierSnapshot);
    }

    if (g_events.size() >= MAX_PENDING_EVENTS) {
        TimingLogEvent(g_events.front(), "drop-sniffer-queue-full");
        g_events.erase(g_events.begin());
    }

    g_events.push_back(event);
    TimingLogEvent(event, "sniffed");
}

static void ProcessKill(BYTE* data, DWORD size, DWORD rawTick) {
    if (size < KILL_MSG_SIZE) return;

    int victimHandle = *(int*)(data + 2);
    int killerHandle = *(int*)(data + 6);
    if (killerHandle <= 0 || killerHandle >= 0x10000) return;
    if (victimHandle <= 0 || victimHandle >= 0x10000) return;

    if (!RememberKill(killerHandle, victimHandle, rawTick)) {
        return;
    }

    DirectorEvent event = {};
    event.type = DirectorEventType::Kill;
    FillEventTiming(event, rawTick);
    event.killerHandle = killerHandle;
    event.victimHandle = victimHandle;
    event.weaponId = (int)data[1];
    ServerTelemetryKillEvent telemetry = {};
    if (ServerTelemetry_TryFindKill(killerHandle, victimHandle, rawTick, &telemetry)) {
        event.hasServerTelemetry = true;
        event.serverTelemetryFlags = telemetry.flags;
        if (telemetry.weaponId > 0) {
            event.weaponId = telemetry.weaponId;
        }
        strncpy_s(event.serverWeaponName, telemetry.weaponName, _TRUNCATE);
    }
    PushEventLocked(event);
}

static void ProcessFlag(BYTE* data, DWORD size, DWORD rawTick) {
    if (size < STATE_VAR_MSG_SIZE) return;

    int index = *(int*)(data + 1);
    int value = *(int*)(data + 5);
    if (index != FLAG_INDEX_US && index != FLAG_INDEX_VC) return;

    int newUS = g_lastRawUSCarrier;
    int newVC = g_lastRawVCCarrier;
    if (index == FLAG_INDEX_US) newUS = value;
    if (index == FLAG_INDEX_VC) newVC = value;

    if (newUS == g_lastRawUSCarrier && newVC == g_lastRawVCCarrier) {
        return;
    }

    DirectorEvent event = {};
    event.type = DirectorEventType::Flag;
    FillEventTiming(event, rawTick);
    event.previousUSCarrier = g_lastRawUSCarrier;
    event.previousVCCarrier = g_lastRawVCCarrier;
    event.usCarrier = newUS;
    event.vcCarrier = newVC;

    EnrichFlagEvent(event);

    g_lastRawUSCarrier = newUS;
    g_lastRawVCCarrier = newVC;
    PushEventLocked(event);
}

// ============================================================================
// Public API
// ============================================================================

void InitServerStateSniffer(uintptr_t gameBase) {
    std::lock_guard<std::mutex> lock(g_snifferMutex);

    g_gameBase = gameBase;
    g_nextSequence = 0;
    g_events.clear();
    g_snapshotHead = 0;
    g_snapshotCount = 0;
    memset(g_snapshotRing, 0, sizeof(g_snapshotRing));
    g_recentKills.clear();
    g_lastKillCleanup = GetTickCount();
    g_lastRawUSCarrier = 0;
    g_lastRawVCCarrier = 0;

    DiagnosticsLog_Append(
        "camera_timing_debug.log",
        "[Sniffer] initialized gameBase=0x%08X localBuffer=%dms gameDelay=%ds\n",
        (unsigned)g_gameBase,
        GetTickDelayMs(),
        DelayManager::GetGameDelaySeconds());
}

void ShutdownServerStateSniffer() {
    std::lock_guard<std::mutex> lock(g_snifferMutex);
    g_events.clear();
    g_snapshotHead = 0;
    g_snapshotCount = 0;
    g_recentKills.clear();
}

void ServerStateSniffer_OnRawMessage(BYTE* data, DWORD size) {
    if (!data || size == 0) return;

    DWORD rawTick = GetTickCount();
    std::lock_guard<std::mutex> lock(g_snifferMutex);

    switch (data[0]) {
        case MSG_KILL:
            ProcessKill(data, size, rawTick);
            break;
        case MSG_STATE_VAR:
            ProcessFlag(data, size, rawTick);
            break;
        case MSG_PLAYER_STATUS:
            ProcessPlayerStatus(data, size, rawTick);
            break;
        default:
            break;
    }
}

void ServerStateSniffer_DrainEvents(std::vector<DirectorEvent>& out) {
    std::lock_guard<std::mutex> lock(g_snifferMutex);
    if (g_events.empty()) return;

    out.insert(out.end(), g_events.begin(), g_events.end());
    g_events.clear();
}

bool ServerStateSniffer_GetSnapshotAt(int handle, DWORD visibleTick, PlayerSnapshot* out) {
    std::lock_guard<std::mutex> lock(g_snifferMutex);
    return GetSnapshotAtLocked(handle, visibleTick, out);
}
