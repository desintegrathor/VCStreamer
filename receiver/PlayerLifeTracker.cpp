#define _CRT_SECURE_NO_WARNINGS
#include "PlayerLifeTracker.h"
#include "DiagnosticsLog.h"
#include "SpectatorController.h"

#include <mutex>
#include <unordered_map>

namespace {

constexpr DWORD kRecentlyDiedMs = 4500;
constexpr uintptr_t kPlayerTableOffset = 0x7AE9C8;
constexpr uintptr_t kPlayerEntityOffset = 244;
constexpr uintptr_t kEntityDeadFlagsOffset = 0xC8;
constexpr int kEntityDeadFlag = 0x20000;

struct LifeRecord {
    PlayerLifeState state;
    int lastScoreboardDeaths = -1;
};

std::mutex g_mutex;
uintptr_t g_gameBase = 0;
std::unordered_map<int, LifeRecord> g_life;

bool TickReached(DWORD now, DWORD tick) {
    return static_cast<LONG>(now - tick) >= 0;
}

LifeRecord& RecordForHandleLocked(int handle) {
    return g_life[handle];
}

void MarkAliveLocked(int handle, DWORD now, const char* source) {
    if (handle <= 0) return;

    LifeRecord& record = RecordForHandleLocked(handle);
    if (record.state.deathTick != 0) {
        if (!TickReached(now, record.state.deathTick)) {
            record.state.status = PlayerLifeStatus::Alive;
            record.state.lastConfirmedAliveTick = now;
            return;
        }
        if (!TickReached(now, record.state.deathTick + kRecentlyDiedMs)) {
            record.state.status = PlayerLifeStatus::RecentlyDied;
            return;
        }
    }

    PlayerLifeStatus previous = record.state.status;
    record.state.status = PlayerLifeStatus::Alive;
    record.state.lastConfirmedAliveTick = now;
    record.state.deathTick = 0;
    record.state.killerHandle = 0;

    if (previous != PlayerLifeStatus::Alive) {
        DiagnosticsLog_Append("camera_timing_debug.log",
                              "[Life] alive handle=%d source=%s now=%lu\n",
                              handle,
                              source ? source : "unknown",
                              now);
    }
}

void MarkDeadLocked(int victimHandle, int killerHandle, DWORD deathTick, const char* source) {
    if (victimHandle <= 0) return;
    DWORD now = GetTickCount();
    if (deathTick == 0) deathTick = now;

    LifeRecord& record = RecordForHandleLocked(victimHandle);
    record.state.status = TickReached(now, deathTick)
        ? PlayerLifeStatus::RecentlyDied
        : PlayerLifeStatus::Alive;
    record.state.deathTick = deathTick;
    record.state.killerHandle = killerHandle;

    DiagnosticsLog_Append("camera_timing_debug.log",
                          "[Life] death victim=%d killer=%d deathTick=%lu now=%lu source=%s\n",
                          victimHandle,
                          killerHandle,
                          deathTick,
                          now,
                          source ? source : "unknown");
}

bool TryReadEntityDeadFlag(uintptr_t entity, bool* outDead) {
    if (!entity || !outDead) return false;

    __try {
        int flags = *(int*)(entity + kEntityDeadFlagsOffset);
        *outDead = (flags & kEntityDeadFlag) != 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryReadPlayerEntityAtIndex(int index, int* outHandle, uintptr_t* outEntity) {
    if (!g_gameBase || !outHandle || !outEntity) return false;

    __try {
        void** playerTable = (void**)(g_gameBase + kPlayerTableOffset);
        void* entry = playerTable[index];
        if (!entry) return false;

        int handle = *(int*)entry;
        if (handle <= 0) return false;

        uintptr_t entity = *(uintptr_t*)((uintptr_t)entry + kPlayerEntityOffset);
        if (!entity) return false;

        *outHandle = handle;
        *outEntity = entity;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void InitPlayerLifeTracker(uintptr_t gameBase) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gameBase = gameBase;
    g_life.clear();
    DiagnosticsLog_Append("camera_timing_debug.log",
                          "[Life] initialized gameBase=0x%08X\n",
                          (unsigned)g_gameBase);
}

void PlayerLifeTracker_OnKill(const DirectorEvent& event) {
    if (event.type != DirectorEventType::Kill) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    MarkDeadLocked(event.victimHandle, event.killerHandle, event.visibleTick, "kill-event");
    if (event.killerHandle > 0 && event.killerHandle != event.victimHandle) {
        DWORD aliveTick = event.visibleTick != 0 ? event.visibleTick : GetTickCount();
        MarkAliveLocked(event.killerHandle, aliveTick, "kill-event-killer");
    }
}

void PlayerLifeTracker_UpdateFromScoreboard(const std::vector<PlayerInfo>& players) {
    DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_mutex);

    for (const PlayerInfo& player : players) {
        if (player.id <= 0) continue;
        LifeRecord& record = RecordForHandleLocked(player.id);
        if (record.lastScoreboardDeaths >= 0
            && player.deaths > record.lastScoreboardDeaths) {
            MarkDeadLocked(player.id, 0, now, "scoreboard-death");
        }
        record.lastScoreboardDeaths = player.deaths;
    }
}

void PlayerLifeTracker_UpdateFromFrame(DWORD now) {
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& item : g_life) {
        PlayerLifeState& state = item.second.state;
        if (state.deathTick != 0 && TickReached(now, state.deathTick)) {
            if (TickReached(now, state.deathTick + kRecentlyDiedMs)) {
                if (state.status == PlayerLifeStatus::RecentlyDied) {
                    state.status = PlayerLifeStatus::RespawningOrUnknown;
                }
            } else {
                state.status = PlayerLifeStatus::RecentlyDied;
            }
        }
    }

    if (!g_gameBase) return;

    for (int i = 0; i < 64; ++i) {
        int handle = 0;
        uintptr_t entity = 0;
        if (!TryReadPlayerEntityAtIndex(i, &handle, &entity)) continue;

        bool dead = false;
        if (!TryReadEntityDeadFlag(entity, &dead)) continue;

        if (dead) {
            LifeRecord& record = RecordForHandleLocked(handle);
            if (record.state.deathTick == 0
                || record.state.status == PlayerLifeStatus::Alive) {
                MarkDeadLocked(handle, record.state.killerHandle, now, "entity-dead-flag");
            }
        } else {
            MarkAliveLocked(handle, now, "entity-dead-flag");
        }
    }
}

PlayerLifeState PlayerLifeTracker_Get(int handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_life.find(handle);
    if (it == g_life.end()) {
        PlayerLifeState unknown = {};
        return unknown;
    }
    return it->second.state;
}

bool PlayerLifeTracker_IsAlive(int handle) {
    if (handle <= 0) return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_life.find(handle);
    if (it == g_life.end()) {
        return true;
    }
    const PlayerLifeState& state = it->second.state;
    if (state.status == PlayerLifeStatus::Alive) {
        return true;
    }
    return state.status == PlayerLifeStatus::RespawningOrUnknown
        && state.deathTick == 0;
}
