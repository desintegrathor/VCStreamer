#include "ServerTelemetry.h"
#include "DiagnosticsLog.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>

namespace {

constexpr unsigned short TELEMETRY_PAYLOAD_VERSION = 1;
constexpr size_t MAX_CACHE_EVENTS = 512;

#pragma pack(push, 1)
struct VchdKillPayload {
    unsigned short payloadVersion;
    unsigned short flags;
    unsigned int eventTick;
    int killerId;
    int victimId;
    short killerTeam;
    short victimTeam;
    unsigned short weaponId;
    unsigned short reserved;
    char killerName[STREAMER_PLAYER_NAME_BYTES];
    char victimName[STREAMER_PLAYER_NAME_BYTES];
    char weaponName[STREAMER_WEAPON_NAME_BYTES];
};

struct VchdHitPayload {
    unsigned short payloadVersion;
    unsigned short flags;
    unsigned int eventTick;
    int attackerId;
    int victimId;
    short attackerTeam;
    short victimTeam;
    unsigned short weaponId;
    unsigned short reserved;
    float damageHP;
    float distanceMeters;
    unsigned int boneMask;
    char attackerName[STREAMER_PLAYER_NAME_BYTES];
    char victimName[STREAMER_PLAYER_NAME_BYTES];
    char weaponName[STREAMER_WEAPON_NAME_BYTES];
};

struct VchdStatsPayload {
    unsigned short payloadVersion;
    unsigned short flags;
    unsigned int eventTick;
    int playerId;
    short team;
    unsigned short reserved;
    char playerName[STREAMER_PLAYER_NAME_BYTES];
    unsigned int teamkills;
    unsigned int flagAttempts;
    unsigned int flagTimeSec;
    unsigned int aliveTimeSec;
    unsigned int distanceMeters;
    unsigned int flagDistanceMeters;
    unsigned int accuracyShots;
    unsigned int accuracyHits;
    unsigned int accuracyHeadshots;
    unsigned int totalDamage;
};

struct VchdFlagPayload {
    unsigned short payloadVersion;
    unsigned short flags;
    unsigned int eventTick;
    int playerId;
    short playerTeam;
    unsigned char flagSide;
    unsigned char action;
    unsigned int carryTimeMs;
    float carryDistanceMeters;
    char playerName[STREAMER_PLAYER_NAME_BYTES];
};

struct VchdAchievementPayload {
    unsigned short payloadVersion;
    unsigned short flags;
    unsigned int eventTick;
    int playerId;
    short playerTeam;
    unsigned short achievementId;
    unsigned short triggerCount;
    unsigned short reserved;
    char playerName[STREAMER_PLAYER_NAME_BYTES];
};

struct VchdKillRecord {
    unsigned long long sequence;
    VchdKillPayload payload;
};

struct VchdHitRecord {
    unsigned long long sequence;
    VchdHitPayload payload;
};

struct VchdStatsRecord {
    unsigned long long sequence;
    VchdStatsPayload payload;
};

struct VchdFlagRecord {
    unsigned long long sequence;
    VchdFlagPayload payload;
};

struct VchdAchievementRecord {
    unsigned long long sequence;
    VchdAchievementPayload payload;
};
#pragma pack(pop)

static_assert(sizeof(VchdKillPayload) == 104, "VCHD kill payload layout mismatch");
static_assert(sizeof(VchdHitPayload) == 116, "VCHD hit payload layout mismatch");
static_assert(sizeof(VchdStatsPayload) == 80, "VCHD stats payload layout mismatch");
static_assert(sizeof(VchdFlagPayload) == 48, "VCHD flag payload layout mismatch");
static_assert(sizeof(VchdAchievementPayload) == 44, "VCHD achievement payload layout mismatch");

using CopyKillEventsFn = unsigned int(__cdecl*)(unsigned long long, VchdKillRecord*, unsigned int);
using CopyHitEventsFn = unsigned int(__cdecl*)(unsigned long long, VchdHitRecord*, unsigned int);
using CopyStatsDeltasFn = unsigned int(__cdecl*)(unsigned long long, VchdStatsRecord*, unsigned int);
using CopyFlagEventsFn = unsigned int(__cdecl*)(unsigned long long, VchdFlagRecord*, unsigned int);
using CopyAchievementEventsFn = unsigned int(__cdecl*)(unsigned long long, VchdAchievementRecord*, unsigned int);
using TelemetryEpochFn = unsigned int(__cdecl*)();
using StreamerIsReadyFn = int(__cdecl*)();

struct VchdTelemetryApi {
    HMODULE module = nullptr;
    StreamerIsReadyFn isReady = nullptr;
    TelemetryEpochFn telemetryEpoch = nullptr;
    CopyKillEventsFn copyKills = nullptr;
    CopyHitEventsFn copyHits = nullptr;
    CopyStatsDeltasFn copyStats = nullptr;
    CopyFlagEventsFn copyFlags = nullptr;
    CopyAchievementEventsFn copyAchievements = nullptr;
};

std::mutex g_mutex;
VchdTelemetryApi g_api;
bool g_started = false;
unsigned long long g_lastKillSeq = 0;
unsigned long long g_lastHitSeq = 0;
unsigned long long g_lastStatsSeq = 0;
unsigned long long g_lastFlagSeq = 0;
unsigned long long g_lastAchievementSeq = 0;
unsigned int g_lastEpoch = 0;
std::deque<ServerTelemetryKillEvent> g_kills;
std::deque<ServerTelemetryHitEvent> g_hits;
std::deque<ServerTelemetryStatsDelta> g_stats;
std::deque<ServerTelemetryFlagEvent> g_flags;
std::deque<ServerTelemetryAchievementEvent> g_achievements;

void ClearTelemetryCacheLocked() {
    g_lastKillSeq = 0;
    g_lastHitSeq = 0;
    g_lastStatsSeq = 0;
    g_lastFlagSeq = 0;
    g_lastAchievementSeq = 0;
    g_kills.clear();
    g_hits.clear();
    g_stats.clear();
    g_flags.clear();
    g_achievements.clear();
}

template <typename T>
void PushBounded(std::deque<T>& queue, const T& value) {
    while (queue.size() >= MAX_CACHE_EVENTS) {
        queue.pop_front();
    }
    queue.push_back(value);
}

bool ResolveApi() {
    HMODULE module = GetModuleHandleA("dinput8.dll");
    if (!module) {
        module = GetModuleHandleA("vchd_dinput8.dll");
    }
    if (!module) {
        return false;
    }
    if (g_api.module == module && g_api.isReady && g_api.telemetryEpoch &&
        g_api.copyKills && g_api.copyHits && g_api.copyStats) {
        return true;
    }

    VchdTelemetryApi next = {};
    next.module = module;
    next.isReady = reinterpret_cast<StreamerIsReadyFn>(
        GetProcAddress(module, "VCHD_StreamerIsReady"));
    next.telemetryEpoch = reinterpret_cast<TelemetryEpochFn>(
        GetProcAddress(module, "VCHD_StreamerTelemetryEpoch"));
    next.copyKills = reinterpret_cast<CopyKillEventsFn>(
        GetProcAddress(module, "VCHD_StreamerCopyKillEvents"));
    next.copyHits = reinterpret_cast<CopyHitEventsFn>(
        GetProcAddress(module, "VCHD_StreamerCopyHitEvents"));
    next.copyStats = reinterpret_cast<CopyStatsDeltasFn>(
        GetProcAddress(module, "VCHD_StreamerCopyStatsDeltas"));
    next.copyFlags = reinterpret_cast<CopyFlagEventsFn>(
        GetProcAddress(module, "VCHD_StreamerCopyFlagEvents"));
    next.copyAchievements = reinterpret_cast<CopyAchievementEventsFn>(
        GetProcAddress(module, "VCHD_StreamerCopyAchievementEvents"));
    if (!next.isReady || !next.telemetryEpoch || !next.copyKills || !next.copyHits || !next.copyStats) {
        return false;
    }

    g_api = next;
    DiagnosticsLog_Append("server_telemetry_debug.log", "[ServerTelemetry] VCHD exports resolved\n");
    return true;
}

ServerTelemetryKillEvent ConvertKill(const VchdKillRecord& record) {
    ServerTelemetryKillEvent out = {};
    out.sequence = record.sequence;
    out.eventTick = record.payload.eventTick;
    out.killerId = record.payload.killerId;
    out.victimId = record.payload.victimId;
    out.killerTeam = record.payload.killerTeam;
    out.victimTeam = record.payload.victimTeam;
    out.weaponId = record.payload.weaponId;
    out.flags = record.payload.flags;
    strncpy_s(out.killerName, record.payload.killerName, _TRUNCATE);
    strncpy_s(out.victimName, record.payload.victimName, _TRUNCATE);
    strncpy_s(out.weaponName, record.payload.weaponName, _TRUNCATE);
    return out;
}

ServerTelemetryHitEvent ConvertHit(const VchdHitRecord& record) {
    ServerTelemetryHitEvent out = {};
    out.sequence = record.sequence;
    out.eventTick = record.payload.eventTick;
    out.attackerId = record.payload.attackerId;
    out.victimId = record.payload.victimId;
    out.attackerTeam = record.payload.attackerTeam;
    out.victimTeam = record.payload.victimTeam;
    out.weaponId = record.payload.weaponId;
    out.flags = record.payload.flags;
    out.damageHP = record.payload.damageHP;
    out.distanceMeters = record.payload.distanceMeters;
    out.boneMask = record.payload.boneMask;
    strncpy_s(out.attackerName, record.payload.attackerName, _TRUNCATE);
    strncpy_s(out.victimName, record.payload.victimName, _TRUNCATE);
    strncpy_s(out.weaponName, record.payload.weaponName, _TRUNCATE);
    return out;
}

ServerTelemetryStatsDelta ConvertStats(const VchdStatsRecord& record) {
    ServerTelemetryStatsDelta out = {};
    out.sequence = record.sequence;
    out.eventTick = record.payload.eventTick;
    out.playerId = record.payload.playerId;
    out.team = record.payload.team;
    strncpy_s(out.playerName, record.payload.playerName, _TRUNCATE);
    out.teamkills = record.payload.teamkills;
    out.flagAttempts = record.payload.flagAttempts;
    out.flagTimeSec = record.payload.flagTimeSec;
    out.aliveTimeSec = record.payload.aliveTimeSec;
    out.distanceMeters = record.payload.distanceMeters;
    out.flagDistanceMeters = record.payload.flagDistanceMeters;
    out.accuracyShots = record.payload.accuracyShots;
    out.accuracyHits = record.payload.accuracyHits;
    out.accuracyHeadshots = record.payload.accuracyHeadshots;
    out.totalDamage = record.payload.totalDamage;
    return out;
}

ServerTelemetryFlagEvent ConvertFlag(const VchdFlagRecord& record) {
    ServerTelemetryFlagEvent out = {};
    out.sequence = record.sequence;
    out.eventTick = record.payload.eventTick;
    out.playerId = record.payload.playerId;
    out.playerTeam = record.payload.playerTeam;
    out.flags = record.payload.flags;
    out.flagSide = record.payload.flagSide;
    out.action = record.payload.action;
    out.carryTimeMs = record.payload.carryTimeMs;
    out.carryDistanceMeters = record.payload.carryDistanceMeters;
    strncpy_s(out.playerName, record.payload.playerName, _TRUNCATE);
    return out;
}

ServerTelemetryAchievementEvent ConvertAchievement(const VchdAchievementRecord& record) {
    ServerTelemetryAchievementEvent out = {};
    out.sequence = record.sequence;
    out.eventTick = record.payload.eventTick;
    out.playerId = record.payload.playerId;
    out.playerTeam = record.payload.playerTeam;
    out.flags = record.payload.flags;
    out.achievementId = record.payload.achievementId;
    out.triggerCount = record.payload.triggerCount;
    strncpy_s(out.playerName, record.payload.playerName, _TRUNCATE);
    return out;
}

}

void InitServerTelemetry(uintptr_t gameBase) {
    (void)gameBase;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_started = true;
    ClearTelemetryCacheLocked();
    g_lastEpoch = 0;
    DiagnosticsLog_Append("server_telemetry_debug.log", "[ServerTelemetry] initialized\n");
}

void ServerTelemetry_Poll() {
    if (!g_started || !ResolveApi()) {
        return;
    }

    VchdKillRecord killRecords[32] = {};
    VchdHitRecord hitRecords[32] = {};
    VchdStatsRecord statsRecords[64] = {};
    VchdFlagRecord flagRecords[32] = {};
    VchdAchievementRecord achievementRecords[32] = {};

    bool ready = g_api.isReady() != 0;
    unsigned int epoch = g_api.telemetryEpoch();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!ready) {
            if (g_lastEpoch != 0) {
                ClearTelemetryCacheLocked();
                g_lastEpoch = 0;
                DiagnosticsLog_Append("server_telemetry_debug.log",
                    "[ServerTelemetry] VCHD not ready, cache cleared\n");
            }
            return;
        }
        if (g_lastEpoch != 0 && epoch != 0 && epoch != g_lastEpoch) {
            ClearTelemetryCacheLocked();
            DiagnosticsLog_Append("server_telemetry_debug.log",
                "[ServerTelemetry] epoch changed %u -> %u, cache cleared\n",
                g_lastEpoch,
                epoch);
        }
        if (epoch != 0) {
            g_lastEpoch = epoch;
        }
    }

    DWORD receivedTick = GetTickCount();
    unsigned int killCount = g_api.copyKills(g_lastKillSeq, killRecords, 32);
    unsigned int hitCount = g_api.copyHits(g_lastHitSeq, hitRecords, 32);
    unsigned int statsCount = g_api.copyStats(g_lastStatsSeq, statsRecords, 64);
    unsigned int flagCount = g_api.copyFlags ? g_api.copyFlags(g_lastFlagSeq, flagRecords, 32) : 0;
    unsigned int achievementCount = g_api.copyAchievements
        ? g_api.copyAchievements(g_lastAchievementSeq, achievementRecords, 32)
        : 0;

    if (killCount == 0 && hitCount == 0 && statsCount == 0 &&
        flagCount == 0 && achievementCount == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (unsigned int i = 0; i < killCount; ++i) {
        if (killRecords[i].payload.payloadVersion != TELEMETRY_PAYLOAD_VERSION) {
            continue;
        }
        ServerTelemetryKillEvent ev = ConvertKill(killRecords[i]);
        ev.receivedTick = receivedTick;
        g_lastKillSeq = (std::max)(g_lastKillSeq, ev.sequence);
        PushBounded(g_kills, ev);
        DiagnosticsLog_Append("server_telemetry_debug.log",
            "[ServerTelemetry] kill seq=%llu killer=%d victim=%d weapon=%d %s\n",
            ev.sequence, ev.killerId, ev.victimId, ev.weaponId, ev.weaponName);
    }
    for (unsigned int i = 0; i < hitCount; ++i) {
        if (hitRecords[i].payload.payloadVersion != TELEMETRY_PAYLOAD_VERSION) {
            continue;
        }
        ServerTelemetryHitEvent ev = ConvertHit(hitRecords[i]);
        ev.receivedTick = receivedTick;
        g_lastHitSeq = (std::max)(g_lastHitSeq, ev.sequence);
        PushBounded(g_hits, ev);
    }
    for (unsigned int i = 0; i < statsCount; ++i) {
        if (statsRecords[i].payload.payloadVersion != TELEMETRY_PAYLOAD_VERSION) {
            continue;
        }
        ServerTelemetryStatsDelta ev = ConvertStats(statsRecords[i]);
        ev.receivedTick = receivedTick;
        g_lastStatsSeq = (std::max)(g_lastStatsSeq, ev.sequence);
        PushBounded(g_stats, ev);
    }
    for (unsigned int i = 0; i < flagCount; ++i) {
        if (flagRecords[i].payload.payloadVersion != TELEMETRY_PAYLOAD_VERSION) {
            continue;
        }
        ServerTelemetryFlagEvent ev = ConvertFlag(flagRecords[i]);
        ev.receivedTick = receivedTick;
        g_lastFlagSeq = (std::max)(g_lastFlagSeq, ev.sequence);
        PushBounded(g_flags, ev);
        DiagnosticsLog_Append("server_telemetry_debug.log",
            "[ServerTelemetry] flag seq=%llu player=%d action=%u flags=%u dist=%.1f\n",
            ev.sequence, ev.playerId, ev.action, ev.flags, ev.carryDistanceMeters);
    }
    for (unsigned int i = 0; i < achievementCount; ++i) {
        if (achievementRecords[i].payload.payloadVersion != TELEMETRY_PAYLOAD_VERSION) {
            continue;
        }
        ServerTelemetryAchievementEvent ev = ConvertAchievement(achievementRecords[i]);
        ev.receivedTick = receivedTick;
        g_lastAchievementSeq = (std::max)(g_lastAchievementSeq, ev.sequence);
        PushBounded(g_achievements, ev);
        DiagnosticsLog_Append("server_telemetry_debug.log",
            "[ServerTelemetry] achievement seq=%llu player=%d id=%u count=%u\n",
            ev.sequence, ev.playerId, ev.achievementId, ev.triggerCount);
    }
}

bool ServerTelemetry_TryFindKill(int killerId,
                                 int victimId,
                                 DWORD localTick,
                                 ServerTelemetryKillEvent* outEvent) {
    if (!outEvent) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    bool found = false;
    ServerTelemetryKillEvent best = {};
    DWORD bestDiff = 0xFFFFFFFF;
    for (const auto& ev : g_kills) {
        if (ev.killerId != killerId || ev.victimId != victimId) {
            continue;
        }
        DWORD matchTick = ev.receivedTick != 0 ? ev.receivedTick : ev.eventTick;
        DWORD diff = matchTick > localTick ? matchTick - localTick : localTick - matchTick;
        if (diff > 5000) {
            continue;
        }
        if (!found || diff < bestDiff) {
            found = true;
            best = ev;
            bestDiff = diff;
        }
    }
    if (!found) {
        return false;
    }
    *outEvent = best;
    return true;
}

bool ServerTelemetry_TryFindFlag(int playerId,
                                 unsigned int action,
                                 DWORD localTick,
                                 ServerTelemetryFlagEvent* outEvent) {
    if (!outEvent) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    bool found = false;
    ServerTelemetryFlagEvent best = {};
    DWORD bestDiff = 0xFFFFFFFF;
    for (const auto& ev : g_flags) {
        if (playerId > 0 && ev.playerId != playerId) {
            continue;
        }
        if (action > 0 && ev.action != action) {
            continue;
        }
        DWORD matchTick = ev.receivedTick != 0 ? ev.receivedTick : ev.eventTick;
        DWORD diff = matchTick > localTick ? matchTick - localTick : localTick - matchTick;
        if (diff > 5000) {
            continue;
        }
        if (!found || diff < bestDiff) {
            found = true;
            best = ev;
            bestDiff = diff;
        }
    }
    if (!found) {
        return false;
    }
    *outEvent = best;
    return true;
}

bool ServerTelemetry_TryFindAchievement(int playerId,
                                        DWORD localTick,
                                        ServerTelemetryAchievementEvent* outEvent) {
    if (!outEvent) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    bool found = false;
    ServerTelemetryAchievementEvent best = {};
    DWORD bestDiff = 0xFFFFFFFF;
    for (const auto& ev : g_achievements) {
        if (playerId > 0 && ev.playerId != playerId) {
            continue;
        }
        DWORD matchTick = ev.receivedTick != 0 ? ev.receivedTick : ev.eventTick;
        DWORD diff = matchTick > localTick ? matchTick - localTick : localTick - matchTick;
        if (diff > 5000) {
            continue;
        }
        if (!found || diff < bestDiff) {
            found = true;
            best = ev;
            bestDiff = diff;
        }
    }
    if (!found) {
        return false;
    }
    *outEvent = best;
    return true;
}

void ServerTelemetry_CopyKills(std::vector<ServerTelemetryKillEvent>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.assign(g_kills.begin(), g_kills.end());
}

void ServerTelemetry_CopyHits(std::vector<ServerTelemetryHitEvent>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.assign(g_hits.begin(), g_hits.end());
}

void ServerTelemetry_CopyStats(std::vector<ServerTelemetryStatsDelta>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.assign(g_stats.begin(), g_stats.end());
}

void ServerTelemetry_CopyFlags(std::vector<ServerTelemetryFlagEvent>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.assign(g_flags.begin(), g_flags.end());
}

void ServerTelemetry_CopyAchievements(std::vector<ServerTelemetryAchievementEvent>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.assign(g_achievements.begin(), g_achievements.end());
}
