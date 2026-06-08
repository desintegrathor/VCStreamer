#define _CRT_SECURE_NO_WARNINGS
#include "CameraDirector.h"
#include "CommentaryEngine.h"
#include "CommentatorFeed.h"
#include "DelayManager.h"
#include "DiagnosticsLog.h"
#include "DroneCamera.h"
#include "PathGrid.h"
#include "PlayerLifeTracker.h"
#include "ServerTelemetry.h"
#include "ServerStateSniffer.h"
#include "SpectatedPlayerData.h"
#include "SpectatorController.h"
#include "WorldCameraTracker.h"
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>

// ============================================================================
// Internal State
// ============================================================================

static uintptr_t g_gameBase = 0;
static CameraConfig g_config;
static CameraState g_state = CameraState::Idle;
static std::mutex g_directorMutex;

// Current target
static int g_currentTargetHandle = 0;
static DWORD g_shotHoldUntil = 0;
static bool g_currentShotUseFpv = false;

static constexpr float NORMAL_SHOT_MIN_HOLD_SEC = 12.0f;
static constexpr float NORMAL_SHOT_MAX_HOLD_SEC = 18.0f;
static constexpr float FLAG_ALTERNATE_MIN_SEC = 8.0f;
static constexpr float FLAG_ALTERNATE_MAX_SEC = 12.0f;
static constexpr DWORD FLAG_SCORE_POST_HOLD_MS = 3000;
static constexpr DWORD FOCUS_DEATH_HOLD_MS = 1800;

// KillCam state
static KillCamPhase g_killCamPhase = KillCamPhase::Wait;
static KillCamStyle g_kcStyle = KillCamStyle::BulletTravel;
static int g_kcKillerHandle = 0;
static int g_kcVictimHandle = 0;
static float g_kcVictimAimPoint[3] = {};
static bool g_kcVictimAimPointValid = false;
static DWORD g_kcStartTick = 0;

// Flag state
static int g_flagCarrierUS = 0;
static int g_flagCarrierVC = 0;
static DWORD g_flagAlternateTimer = 0;
static float g_flagAlternateDelay = 10.0f;
static DWORD g_lastFlagWatchTargetSwitch = 0;
static int g_flagScoreHoldTarget = 0;
static DWORD g_flagScoreHoldUntil = 0;
static int g_flagKillLookKillerHandle = 0;
static int g_flagKillLookVictimHandle = 0;
static DWORD g_flagKillLookStartTick = 0;
static DWORD g_flagKillLookKillTick = 0;
static float g_flagKillLookKillerAimPoint[3] = {};
static bool g_flagKillLookKillerAimPointValid = false;
struct FlagCarrierMotion {
    int handle = 0;
    bool initialized = false;
    bool camping = false;
    float anchorPos[3] = {};
    DWORD anchorTick = 0;
    DWORD lastGlimpseTick = 0;
};
static FlagCarrierMotion g_flagMotionUS;
static FlagCarrierMotion g_flagMotionVC;
static int g_flagCampingGlimpseHandle = 0;
static DWORD g_flagCampingGlimpseUntil = 0;

// Cooldowns
static DWORD g_lastKillSwitch = 0;
static DWORD g_flagLostTimestamp = 0;
static bool g_flagLostGraceActive = false;

// Idle tracking for drone activation
static DWORD g_lastEventTick = 0;

// Screen time tracking (seconds)
static float g_playerCamTime = 0.0f;
static float g_worldCamTime = 0.0f;
static float g_droneCamTime = 0.0f;
static DWORD g_lastUpdateTick = 0;
static DWORD g_currentHoldStart = 0;
static DWORD g_lastScreenTimeLog = 0;

struct PlayerActivity {
    int handle = 0;
    bool initialized = false;
    float lastPos[3] = {};
    DWORD lastSeen = 0;
    float score = 0.0f;
};

static PlayerActivity g_playerActivity[64];

// Camera type enum for deficit scheduler
enum CamBudgetType { CAM_PLAYER = 0, CAM_WORLD = 1, CAM_DRONE = 2 };

enum class ShotKind {
    None,
    FollowPlayer,
    KillCam,
    FlagWatch,
    Drone
};

struct ShotRequest {
    ShotKind kind;
    int targetHandle;
    int victimHandle;
    KillCamStyle killCamStyle;
    CamBudgetType preference;
    float holdSeconds;
    bool force;
    const char* reason;
    bool hasVictimAimPoint;
    float victimAimPoint[3];
};

struct PendingDirectorEvent {
    DirectorEvent event;
    DWORD plannedStartTick;
    bool dueLogged;
};

static ShotKind g_committedShotKind = ShotKind::None;
static CamBudgetType g_committedPreference = CAM_WORLD;
static std::vector<PendingDirectorEvent> g_eventQueue;
static DWORD g_currentInvalidSince = 0;
static unsigned long long g_compatEventSequence = 0;
static int g_focusDeathTargetHandle = 0;
static int g_focusDeathKillerHandle = 0;
static DWORD g_focusDeathTick = 0;
static DWORD g_focusDeathHoldUntil = 0;

constexpr DWORD KILL_EVENT_STALE_GRACE_MS = 3000;
constexpr DWORD FLAG_EVENT_STALE_GRACE_MS = 30000;
constexpr DWORD TARGET_INVALID_GRACE_MS = 1000;
constexpr size_t MAX_DIRECTOR_EVENT_QUEUE = 128;

static void CD_Log(const char* fmt, ...) {
    if (!DiagnosticsLog_IsEnabled()) return;

    FILE* file = fopen("receiver_debug.log", "a");
    if (!file) return;

    va_list args;
    va_start(args, fmt);
    DiagnosticsLog_Write(file, fmt, args);
    va_end(args);

    fclose(file);
}

static const char* DirectorEventTypeName(DirectorEventType type) {
    return type == DirectorEventType::Flag ? "flag" : "kill";
}

static void TimingLog(const char* fmt, ...) {
    if (!DiagnosticsLog_IsEnabled()) return;

    FILE* file = fopen("camera_timing_debug.log", "a");
    if (!file) return;

    va_list args;
    va_start(args, fmt);
    DiagnosticsLog_Write(file, fmt, args);
    va_end(args);

    fclose(file);
}

// ============================================================================
// Entity helpers (same logic as FirstPersonCamera.cpp)
// ============================================================================

static void* CD_FindPlayerEntryByHandle(int handle) {
    if (!g_gameBase || handle == 0) return nullptr;

    void** playerTable = (void**)(g_gameBase + 0x7AE9C8);
    for (int i = 0; i < 64; i++) {
        if (!playerTable[i]) continue;
        if (*(int*)playerTable[i] == handle) {
            return playerTable[i];
        }
    }
    return nullptr;
}

static void* CD_FindEntityByHandle(int handle) {
    void* entry = CD_FindPlayerEntryByHandle(handle);
    if (!entry) return nullptr;

    uintptr_t entity = *(uintptr_t*)((uintptr_t)entry + 244);
    return entity ? (void*)entity : nullptr;
}

static void CD_GetEntityPos(void* entity, float* out) {
    out[0] = *(float*)((uintptr_t)entity + 0xD0);
    out[1] = *(float*)((uintptr_t)entity + 0xD4);
    out[2] = *(float*)((uintptr_t)entity + 0xD8);
}

static bool CD_NameContainsSpectator(const char* name) {
    if (!name) return true;
    for (int i = 0; i < 64 && name[i] != '\0'; ++i) {
        if (strncmp(name + i, "Spectator", 9) == 0) {
            return true;
        }
    }
    return false;
}

static bool CD_IsUsablePlayerEntry(void* entry) {
    if (!entry) return false;

    __try {
        if (*(int*)entry == 0) return false;
        if (CD_NameContainsSpectator((const char*)entry + 40)) return false;

        uintptr_t entity = *(uintptr_t*)((uintptr_t)entry + 244);
        if (!entity) return false;

        float pos[3] = {
            *(float*)(entity + 0xD0),
            *(float*)(entity + 0xD4),
            *(float*)(entity + 0xD8)
        };
        return !(pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CD_IsUsablePlayerHandle(int handle) {
    return CD_IsUsablePlayerEntry(CD_FindPlayerEntryByHandle(handle));
}

static bool CD_IsAliveUsablePlayerHandle(int handle) {
    return CD_IsUsablePlayerHandle(handle) && PlayerLifeTracker_IsAlive(handle);
}

struct PlayerLabel {
    std::string name;
    std::string team;
};

static const char* TeamNameFromId(int teamId) {
    switch (teamId) {
        case 0: return "US";
        case 1: return "VC";
        default: return "Unknown";
    }
}

static bool ReadPlayerLabelRaw(void* entry, char name[64], int* teamId) {
    if (!entry || !name || !teamId) return false;

    __try {
        *teamId = *((int*)entry + 5);
        const char* rawName = (const char*)entry + 40;
        for (int i = 0; i < 63 && rawName[i] != '\0'; ++i) {
            unsigned char ch = (unsigned char)rawName[i];
            name[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
        name[63] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static PlayerLabel ResolvePlayerLabel(int handle) {
    PlayerLabel label;
    label.name = "#" + std::to_string(handle);
    label.team = "Unknown";

    void* entry = CD_FindPlayerEntryByHandle(handle);
    if (!entry) return label;

    char name[64] = {};
    int teamId = -1;
    if (ReadPlayerLabelRaw(entry, name, &teamId)) {
        if (name[0] != '\0') {
            label.name = name;
        }
        label.team = CD_NameContainsSpectator(name) ? "Spectator" : TeamNameFromId(teamId);
    }

    return label;
}

static const char* FlagNameForCarrier(int handle, int usCarrier, int vcCarrier) {
    if (handle != 0 && handle == usCarrier) return "US flag";
    if (handle != 0 && handle == vcCarrier) return "VC flag";
    if (usCarrier != 0 && vcCarrier == 0) return "US flag";
    if (vcCarrier != 0 && usCarrier == 0) return "VC flag";
    return "flag";
}

static int LeadSeconds(int leadMs) {
    if (leadMs <= 0) return 0;
    int seconds = (leadMs + 500) / 1000;
    return seconds > 0 ? seconds : 1;
}

static float Clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static float PosDistance(const float* a, const float* b) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void BumpPlayerActivity(int handle, float amount) {
    if (handle == 0) return;
    for (PlayerActivity& activity : g_playerActivity) {
        if (activity.initialized && activity.handle == handle) {
            activity.score = Clamp01(activity.score + amount);
            activity.lastSeen = GetTickCount();
            return;
        }
    }
}

static void UpdatePlayerActivity(DWORD now) {
    if (!g_gameBase) return;

    void** playerTable = (void**)(g_gameBase + 0x7AE9C8);
    for (int i = 0; i < 64; ++i) {
        PlayerActivity& activity = g_playerActivity[i];
        void* entry = playerTable[i];

        if (!CD_IsUsablePlayerEntry(entry)) {
            if (activity.initialized && now - activity.lastSeen > 5000) {
                activity.score *= 0.8f;
                if (activity.score < 0.01f) {
                    activity = PlayerActivity();
                }
            }
            continue;
        }

        int handle = *(int*)entry;
        void* entity = (void*)*(uintptr_t*)((uintptr_t)entry + 244);
        float pos[3];
        CD_GetEntityPos(entity, pos);

        if (!activity.initialized || activity.handle != handle) {
            activity.handle = handle;
            activity.initialized = true;
            activity.lastPos[0] = pos[0];
            activity.lastPos[1] = pos[1];
            activity.lastPos[2] = pos[2];
            activity.lastSeen = now;
            activity.score = 0.0f;
            continue;
        }

        float dt = (now - activity.lastSeen) / 1000.0f;
        if (dt > 0.001f && dt < 2.0f) {
            float moved = PosDistance(pos, activity.lastPos);
            float speed = moved / dt;
            float movementScore = Clamp01((speed - 0.12f) / 1.4f);
            float decay = expf(-dt * 0.35f);
            activity.score *= decay;
            if (movementScore > activity.score) {
                activity.score += (movementScore - activity.score) * 0.6f;
            }
        } else if (dt >= 2.0f) {
            activity.score *= 0.5f;
        }

        activity.lastPos[0] = pos[0];
        activity.lastPos[1] = pos[1];
        activity.lastPos[2] = pos[2];
        activity.lastSeen = now;
    }
}

static float ActivityScoreForTableIndex(int index, int handle) {
    if (index < 0 || index >= 64) return 0.0f;
    const PlayerActivity& activity = g_playerActivity[index];
    if (!activity.initialized || activity.handle != handle) return 0.0f;
    if (GetTickCount() - activity.lastSeen > 10000) return 0.0f;
    return activity.score;
}

static float ActivityScoreForHandle(int handle) {
    if (handle == 0) return 0.0f;
    for (int i = 0; i < 64; ++i) {
        if (g_playerActivity[i].initialized && g_playerActivity[i].handle == handle) {
            return ActivityScoreForTableIndex(i, handle);
        }
    }
    return 0.0f;
}

static void ClearFlagCampingGlimpse() {
    g_flagCampingGlimpseHandle = 0;
    g_flagCampingGlimpseUntil = 0;
}

static bool ReadPlayerPosition(int handle, float out[3]) {
    void* entity = CD_FindEntityByHandle(handle);
    if (!entity) return false;
    CD_GetEntityPos(entity, out);
    return !(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);
}

static void UpdateFlagCarrierMotionState(FlagCarrierMotion& motion,
                                         int handle,
                                         DWORD now,
                                         const char* label) {
    if (handle == 0 || !CD_IsAliveUsablePlayerHandle(handle)) {
        if (motion.initialized) {
            motion = FlagCarrierMotion();
        }
        return;
    }

    float pos[3];
    if (!ReadPlayerPosition(handle, pos)) return;

    if (!motion.initialized || motion.handle != handle) {
        motion.handle = handle;
        motion.initialized = true;
        motion.camping = false;
        motion.anchorPos[0] = pos[0];
        motion.anchorPos[1] = pos[1];
        motion.anchorPos[2] = pos[2];
        motion.anchorTick = now;
        motion.lastGlimpseTick = now;
        return;
    }

    float moved = PosDistance(pos, motion.anchorPos);
    float elapsed = (now - motion.anchorTick) / 1000.0f;
    float window = g_config.flagCampingWindow;
    if (window < 1.0f) window = 1.0f;

    if (motion.camping && moved >= g_config.flagCampingDistance) {
        motion.camping = false;
        motion.anchorPos[0] = pos[0];
        motion.anchorPos[1] = pos[1];
        motion.anchorPos[2] = pos[2];
        motion.anchorTick = now;
        motion.lastGlimpseTick = now;
        CD_Log("[CameraDirector] Flag carrier moving again %s=%d moved=%.2fm\n",
               label, handle, moved);
        return;
    }

    if (elapsed >= window) {
        bool isCamping = moved < g_config.flagCampingDistance;
        if (isCamping && !motion.camping) {
            motion.camping = true;
            motion.lastGlimpseTick = now;
            CD_Log("[CameraDirector] Flag carrier camping %s=%d moved=%.2fm/%ds\n",
                   label, handle, moved, (int)window);
        } else if (!isCamping && motion.camping) {
            motion.camping = false;
            motion.lastGlimpseTick = now;
            CD_Log("[CameraDirector] Flag carrier moving again %s=%d moved=%.2fm\n",
                   label, handle, moved);
        }

        motion.anchorPos[0] = pos[0];
        motion.anchorPos[1] = pos[1];
        motion.anchorPos[2] = pos[2];
        motion.anchorTick = now;
    }
}

static void UpdateFlagCarrierMotion(DWORD now) {
    UpdateFlagCarrierMotionState(g_flagMotionUS, g_flagCarrierUS, now, "US");
    UpdateFlagCarrierMotionState(g_flagMotionVC, g_flagCarrierVC, now, "VC");
}

static bool IsCampingFlagCarrier(int handle) {
    if (handle == 0) return false;
    return (g_flagMotionUS.initialized && g_flagMotionUS.handle == handle && g_flagMotionUS.camping)
        || (g_flagMotionVC.initialized && g_flagMotionVC.handle == handle && g_flagMotionVC.camping);
}

static bool IsCurrentFlagCarrierHandle(int handle) {
    return handle != 0 && (handle == g_flagCarrierUS || handle == g_flagCarrierVC);
}

static bool IsUsableActiveFlagCarrier(int handle) {
    return IsCurrentFlagCarrierHandle(handle)
        && CD_IsAliveUsablePlayerHandle(handle)
        && !IsCampingFlagCarrier(handle);
}

static int OtherFlagCarrierHandle(int handle) {
    if (handle == g_flagCarrierUS) return g_flagCarrierVC;
    if (handle == g_flagCarrierVC) return g_flagCarrierUS;
    return g_flagCarrierUS != 0 ? g_flagCarrierUS : g_flagCarrierVC;
}

static FlagCarrierMotion* FlagMotionForHandle(int handle) {
    if (g_flagMotionUS.initialized && g_flagMotionUS.handle == handle) return &g_flagMotionUS;
    if (g_flagMotionVC.initialized && g_flagMotionVC.handle == handle) return &g_flagMotionVC;
    return nullptr;
}

static int PickMovingFlagCarrierTarget() {
    bool usMoving = g_flagCarrierUS != 0
        && CD_IsAliveUsablePlayerHandle(g_flagCarrierUS)
        && !IsCampingFlagCarrier(g_flagCarrierUS);
    bool vcMoving = g_flagCarrierVC != 0
        && CD_IsAliveUsablePlayerHandle(g_flagCarrierVC)
        && !IsCampingFlagCarrier(g_flagCarrierVC);

    if (usMoving && vcMoving) {
        if (g_currentTargetHandle == g_flagCarrierUS
            || g_currentTargetHandle == g_flagCarrierVC) {
            return g_currentTargetHandle;
        }
        return g_flagCarrierUS;
    }
    if (usMoving) return g_flagCarrierUS;
    if (vcMoving) return g_flagCarrierVC;
    return 0;
}

static bool HasActiveAlternative(int avoidHandle) {
    if (!g_gameBase) return false;

    void** playerTable = (void**)(g_gameBase + 0x7AE9C8);
    for (int i = 0; i < 64; ++i) {
        void* entry = playerTable[i];
        if (!CD_IsUsablePlayerEntry(entry)) continue;

        int handle = *(int*)entry;
        if (!PlayerLifeTracker_IsAlive(handle)) continue;
        if (handle == avoidHandle) continue;
        if (IsCampingFlagCarrier(handle)) continue;
        if (ActivityScoreForTableIndex(i, handle) >= 0.12f) {
            return true;
        }
    }
    return false;
}

static int PickRandomActivePlayer(int avoidHandle) {
    if (!g_gameBase) return 0;

    struct Candidate {
        int handle;
        float activity;
    };

    std::vector<Candidate> candidates;
    std::vector<Candidate> fallbackCandidates;
    candidates.reserve(64);
    fallbackCandidates.reserve(64);

    void** playerTable = (void**)(g_gameBase + 0x7AE9C8);
    for (int i = 0; i < 64; ++i) {
        void* entry = playerTable[i];
        if (!CD_IsUsablePlayerEntry(entry)) continue;

        int handle = *(int*)entry;
        if (!PlayerLifeTracker_IsAlive(handle)) continue;
        if (handle != avoidHandle) {
            Candidate candidate = { handle, ActivityScoreForTableIndex(i, handle) };
            if (IsCampingFlagCarrier(handle)) {
                fallbackCandidates.push_back(candidate);
            } else {
                candidates.push_back(candidate);
            }
        }
    }

    if (candidates.empty() && !fallbackCandidates.empty()) {
        candidates.swap(fallbackCandidates);
    }

    if (candidates.empty() && avoidHandle != 0 && CD_IsAliveUsablePlayerHandle(avoidHandle)) {
        return avoidHandle;
    }
    if (candidates.empty()) return 0;

    bool hasActiveCandidate = false;
    for (const Candidate& candidate : candidates) {
        if (candidate.activity >= 0.12f) {
            hasActiveCandidate = true;
            break;
        }
    }

    float totalWeight = 0.0f;
    for (const Candidate& candidate : candidates) {
        if (hasActiveCandidate && candidate.activity < 0.04f) {
            continue;
        }
        totalWeight += hasActiveCandidate
            ? (0.05f + candidate.activity * candidate.activity * 8.0f)
            : 1.0f;
    }

    if (totalWeight <= 0.001f) {
        return candidates[(size_t)(rand() % (int)candidates.size())].handle;
    }

    float pick = ((float)(rand() % 10000) / 10000.0f) * totalWeight;
    for (const Candidate& candidate : candidates) {
        if (hasActiveCandidate && candidate.activity < 0.04f) {
            continue;
        }
        float weight = hasActiveCandidate
            ? (0.05f + candidate.activity * candidate.activity * 8.0f)
            : 1.0f;
        if (pick <= weight) return candidate.handle;
        pick -= weight;
    }

    return candidates.back().handle;
}

static float RandomRange(float minValue, float maxValue) {
    float t = (float)(rand() % 10000) / 10000.0f;
    return minValue + (maxValue - minValue) * t;
}

static DWORD HoldUntil(DWORD now, float holdSeconds) {
    if (holdSeconds < 0.1f) holdSeconds = 0.1f;
    return now + (DWORD)(holdSeconds * 1000.0f);
}

static float MinHoldForPreference(CamBudgetType preference) {
    if (preference == CAM_PLAYER) return g_config.camMinHoldPlayer;
    if (preference == CAM_DRONE) return g_config.camMinHoldDrone;
    return g_config.camMinHoldWorld;
}

static float NormalFollowHoldSeconds(CamBudgetType preference) {
    float holdSeconds = RandomRange(NORMAL_SHOT_MIN_HOLD_SEC, NORMAL_SHOT_MAX_HOLD_SEC);
    float minHold = MinHoldForPreference(preference);
    return holdSeconds > minHold ? holdSeconds : minHold;
}

static bool IsShotHoldActive(DWORD now) {
    return g_shotHoldUntil != 0 && now < g_shotHoldUntil;
}

static bool RollFpvForShot() {
    return false;
}

static bool CurrentViewUsable() {
    if (!CD_IsAliveUsablePlayerHandle(g_currentTargetHandle)) return false;

    int wcType = WorldCameraTracker_GetCurrentCamType();
    if (wcType == 0 || wcType == 1) {
        return WorldCameraTracker_HasUsableWorldCamera();
    }

    return true;
}

static bool CurrentHasGoodWorldView() {
    int wcType = WorldCameraTracker_GetCurrentCamType();
    return (wcType == 0 || wcType == 1) && WorldCameraTracker_HasUsableWorldCamera();
}

// ============================================================================
// Distance-Based Cinematic Chance
// ============================================================================

static bool GetCurrentPlayerPosition(int handle, float* out) {
    void* entity = CD_FindEntityByHandle(handle);
    if (!entity) return false;

    CD_GetEntityPos(entity, out);
    return !(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);
}

static bool CopySnapshotPosition(const PlayerSnapshot& snapshot, float* out) {
    if (!snapshot.hasPosition) return false;

    out[0] = snapshot.pos[0];
    out[1] = snapshot.pos[1];
    out[2] = snapshot.pos[2];
    return true;
}

static bool GetFrozenVictimAimPoint(const DirectorEvent& event, float* out) {
    if (!out || event.type != DirectorEventType::Kill) return false;

    PlayerSnapshot snapshot = {};
    if (event.hasVictimSnapshot && event.victimSnapshot.hasPosition) {
        snapshot = event.victimSnapshot;
    } else if (!ServerStateSniffer_GetSnapshotAt(event.victimHandle,
                                                 event.visibleTick,
                                                 &snapshot)) {
        if (!GetCurrentPlayerPosition(event.victimHandle, out)) {
            return false;
        }

        out[2] += 1.25f;
        return true;
    }

    if (!CopySnapshotPosition(snapshot, out)) {
        return false;
    }

    out[2] += 1.25f;
    return true;
}

static bool GetFrozenKillerAimPoint(const DirectorEvent& event, float* out) {
    if (!out || event.type != DirectorEventType::Kill) return false;

    PlayerSnapshot snapshot = {};
    if (event.hasKillerSnapshot && event.killerSnapshot.hasPosition) {
        snapshot = event.killerSnapshot;
    } else if (!ServerStateSniffer_GetSnapshotAt(event.killerHandle,
                                                 event.visibleTick,
                                                 &snapshot)) {
        if (!GetCurrentPlayerPosition(event.killerHandle, out)) {
            return false;
        }

        out[2] += 1.25f;
        return true;
    }

    if (!CopySnapshotPosition(snapshot, out)) {
        return false;
    }

    out[2] += 1.25f;
    return true;
}

static bool GetBestKillPosition(int handle,
                                DWORD visibleTick,
                                const PlayerSnapshot* eventSnapshot,
                                float* out,
                                bool* usedLiveAhead) {
    if (usedLiveAhead) *usedLiveAhead = false;

    if (eventSnapshot && eventSnapshot->hasPosition) {
        if (CopySnapshotPosition(*eventSnapshot, out)) {
            if (usedLiveAhead) *usedLiveAhead = true;
            return true;
        }
    }

    PlayerSnapshot snapshot = {};
    if (ServerStateSniffer_GetSnapshotAt(handle, visibleTick, &snapshot)
        && CopySnapshotPosition(snapshot, out)) {
        if (usedLiveAhead) *usedLiveAhead = true;
        return true;
    }

    return GetCurrentPlayerPosition(handle, out);
}

static float GetKillDistanceFromEvent(const DirectorEvent& event, bool* usedLiveAhead) {
    if (usedLiveAhead) *usedLiveAhead = false;

    bool killerLiveAhead = false;
    bool victimLiveAhead = false;
    float kp[3], vp[3];
    if (!GetBestKillPosition(event.killerHandle,
                             event.visibleTick,
                             event.hasKillerSnapshot ? &event.killerSnapshot : nullptr,
                             kp,
                             &killerLiveAhead)) {
        return 0.0f;
    }

    if (!GetBestKillPosition(event.victimHandle,
                             event.visibleTick,
                             event.hasVictimSnapshot ? &event.victimSnapshot : nullptr,
                             vp,
                             &victimLiveAhead)) {
        return 0.0f;
    }

    if (usedLiveAhead) *usedLiveAhead = killerLiveAhead || victimLiveAhead;

    float dx = kp[0] - vp[0];
    float dy = kp[1] - vp[1];
    float dz = kp[2] - vp[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float GetKillDistance(int killerHandle, int victimHandle) {
    void* killerEntity = CD_FindEntityByHandle(killerHandle);
    void* victimEntity = CD_FindEntityByHandle(victimHandle);
    if (!killerEntity || !victimEntity) return 0.0f;

    float kp[3], vp[3];
    CD_GetEntityPos(killerEntity, kp);
    CD_GetEntityPos(victimEntity, vp);

    float dx = kp[0] - vp[0];
    float dy = kp[1] - vp[1];
    float dz = kp[2] - vp[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float ComputeCinematicChance(float distance) {
    if (distance < g_config.cinematicMinDistance) return 0.0f;
    if (distance >= g_config.cinematicMaxDistance) return g_config.cinematicMaxChance;

    float t = (distance - g_config.cinematicMinDistance)
            / (g_config.cinematicMaxDistance - g_config.cinematicMinDistance);
    return g_config.cinematicMinChance + t * (g_config.cinematicMaxChance - g_config.cinematicMinChance);
}

static bool RollCinematic(float chance) {
    float roll = (float)(rand() % 1000) / 1000.0f;
    return roll < chance;
}

// ============================================================================
// KillCam Phase Management
// ============================================================================

static float GetKillCamElapsedInternal() {
    return (GetTickCount() - g_kcStartTick) / 1000.0f;
}

static float GetKillCamWaitDuration(KillCamStyle style) {
    return style == KillCamStyle::DetachedVantage
        ? g_config.detachedKillCamFollowDuration
        : g_config.killCamWaitDuration;
}

static float GetKillCamTransitionDuration(KillCamStyle style) {
    return style == KillCamStyle::DetachedVantage
        ? g_config.detachedKillCamRepositionDuration
        : g_config.killCamTransitionDuration;
}

static float GetKillCamAttachedDuration(KillCamStyle style) {
    return style == KillCamStyle::DetachedVantage
        ? g_config.detachedKillCamHoldDuration
        : g_config.killCamAttachedDuration;
}

static float GetKillCamTotalDuration(KillCamStyle style) {
    return GetKillCamWaitDuration(style)
         + GetKillCamTransitionDuration(style)
         + GetKillCamAttachedDuration(style);
}

static float GetKillCamTotalDuration() {
    return GetKillCamTotalDuration(g_kcStyle);
}

static bool TickReached(DWORD now, DWORD tick) {
    return (LONG)(now - tick) >= 0;
}

static bool IsFlagScoreHoldActive(DWORD now) {
    return g_flagScoreHoldTarget != 0
        && g_flagScoreHoldUntil != 0
        && !TickReached(now, g_flagScoreHoldUntil);
}

static void ExtendShotHoldTo(DWORD holdUntil) {
    if (holdUntil == 0) return;
    if (g_shotHoldUntil == 0 || TickReached(holdUntil, g_shotHoldUntil)) {
        g_shotHoldUntil = holdUntil;
    }
}

static void ClearFlagScoreHold(DWORD now, const char* reason) {
    if (g_flagScoreHoldTarget != 0) {
        CD_Log("[CameraDirector] Flag score hold cleared target=%d reason=%s now=%lu\n",
               g_flagScoreHoldTarget,
               reason ? reason : "none",
               now);
    }
    g_flagScoreHoldTarget = 0;
    g_flagScoreHoldUntil = 0;
}

static float GetKillLookLeadDuration() {
    return g_config.detachedKillCamFollowDuration
         + g_config.detachedKillCamRepositionDuration;
}

static float GetKillLookAdvanceDuration() {
    float lead = GetKillLookLeadDuration();
    if (lead < 0.1f) lead = 0.1f;

    float advance = g_config.killLookLockAdvance;
    if (advance < 0.1f) advance = 0.1f;
    if (advance > lead) advance = lead;
    return advance;
}

static void ClearFlagKillLook() {
    g_flagKillLookKillerHandle = 0;
    g_flagKillLookVictimHandle = 0;
    g_flagKillLookStartTick = 0;
    g_flagKillLookKillTick = 0;
    g_flagKillLookKillerAimPointValid = false;
}

static bool MaybeStartVictimKillLook(int killerHandle,
                                     int victimHandle,
                                     DWORD now,
                                     DWORD visibleKillTick,
                                     const float* killerAimPoint) {
    bool victimIsFlagCarrier = (victimHandle != 0)
        && (victimHandle == g_flagCarrierUS || victimHandle == g_flagCarrierVC);
    bool watchingVictim = g_state == CameraState::FlagWatch
        && g_currentTargetHandle == victimHandle;

    if (!watchingVictim || !victimIsFlagCarrier || killerHandle == 0) {
        return false;
    }

    float advanceSeconds = GetKillLookAdvanceDuration();
    DWORD advanceMs = (DWORD)(advanceSeconds * 1000.0f + 0.5f);
    DWORD startTick = visibleKillTick >= advanceMs
        ? visibleKillTick - advanceMs
        : now;
    if (!TickReached(now, startTick)) {
        // Not time to apply the look lock yet; keep the queued event alive.
        return false;
    }

    g_flagKillLookKillerHandle = killerHandle;
    g_flagKillLookVictimHandle = victimHandle;
    g_flagKillLookKillTick = visibleKillTick;
    g_flagKillLookStartTick = startTick;
    g_flagKillLookKillerAimPointValid = killerAimPoint != nullptr;
    if (g_flagKillLookKillerAimPointValid) {
        g_flagKillLookKillerAimPoint[0] = killerAimPoint[0];
        g_flagKillLookKillerAimPoint[1] = killerAimPoint[1];
        g_flagKillLookKillerAimPoint[2] = killerAimPoint[2];
    }

    CD_Log("[CameraDirector] Victim death look: victim=%d killer=%d flagCarrier=%s frozenKillerAim=%s startsIn=%ds, holds until camera changes\n",
           victimHandle,
           killerHandle,
           victimIsFlagCarrier ? "yes" : "no",
           g_flagKillLookKillerAimPointValid ? "yes" : "no",
           0);
    return true;
}

// ============================================================================
// State Transitions
// ============================================================================

// Check if drone is suitable at target player's position
static bool IsDroneSuitable() {
    if (!PathGrid_IsReady()) return true; // no grid data — allow drone
    void* entity = CD_FindEntityByHandle(g_currentTargetHandle);
    if (!entity) return false;
    float pos[3];
    CD_GetEntityPos(entity, pos);
    float clearance = PathGrid_GetAreaClearance(pos[0], pos[1], pos[2], 5.0f);
    return clearance >= g_config.droneMinAreaClearance;
}

static bool IsDroneSuitableForTarget(int targetHandle) {
    if (!PathGrid_IsReady()) return true;
    void* entity = CD_FindEntityByHandle(targetHandle);
    if (!entity) return false;
    float pos[3];
    CD_GetEntityPos(entity, pos);
    float clearance = PathGrid_GetAreaClearance(pos[0], pos[1], pos[2], 5.0f);
    return clearance >= g_config.droneMinAreaClearance;
}

static const char* BudgetName(CamBudgetType preference) {
    if (preference == CAM_PLAYER) return "player";
    if (preference == CAM_DRONE) return "drone";
    return "world";
}

static const char* ShotKindName(ShotKind kind) {
    switch (kind) {
        case ShotKind::FollowPlayer: return "follow";
        case ShotKind::KillCam: return "killcam";
        case ShotKind::FlagWatch: return "flag";
        case ShotKind::Drone: return "drone";
        default: return "none";
    }
}

static CommentaryCameraContext BuildCommentaryContext(DWORD now) {
    CommentaryCameraContext context = {};
    context.currentTick = now;
    context.activeCameraTarget = g_currentTargetHandle;
    context.shotKind = ShotKindName(g_committedShotKind);
    context.usCarrier = g_flagCarrierUS;
    context.vcCarrier = g_flagCarrierVC;
    context.usCarrierCamping = IsCampingFlagCarrier(g_flagCarrierUS);
    context.vcCarrierCamping = IsCampingFlagCarrier(g_flagCarrierVC);
    context.usCarrierMoving = g_flagCarrierUS != 0 && !context.usCarrierCamping;
    context.vcCarrierMoving = g_flagCarrierVC != 0 && !context.vcCarrierCamping;
    context.flagScoreHoldTarget = IsFlagScoreHoldActive(now) ? g_flagScoreHoldTarget : 0;
    context.flagScoreHoldUntil = IsFlagScoreHoldActive(now) ? g_flagScoreHoldUntil : 0;
    context.flagKillLookKillerHandle = g_flagKillLookKillerHandle;
    context.flagKillLookVictimHandle = g_flagKillLookVictimHandle;
    context.flagKillLookStartTick = g_flagKillLookStartTick;
    context.flagKillLookKillTick = g_flagKillLookKillTick;
    context.targetSinceTick = g_currentHoldStart;
    context.targetAlive = true;
    context.targetRecentlyDied = false;
    context.targetDeathKillerHandle = 0;
    context.targetDeathTick = 0;
    if (g_currentTargetHandle != 0) {
        PlayerLifeState life = PlayerLifeTracker_Get(g_currentTargetHandle);
        context.targetAlive = PlayerLifeTracker_IsAlive(g_currentTargetHandle);
        context.targetRecentlyDied = life.status == PlayerLifeStatus::RecentlyDied;
        context.targetDeathKillerHandle = life.killerHandle;
        context.targetDeathTick = life.deathTick;
    }
    return context;
}

static const char* KillCamStyleName(KillCamStyle style) {
    return style == KillCamStyle::DetachedVantage ? "victim-lock-3pv" : "bullet";
}

static const char* DebugCameraModeName(DebugCameraMode mode) {
    switch (mode) {
        case DebugCameraMode::Player3pv: return "player_3pv";
        case DebugCameraMode::Fpv: return "fpv";
        case DebugCameraMode::World: return "world";
        case DebugCameraMode::Drone: return "drone";
        case DebugCameraMode::VictimLook3pv: return "victim_look_3pv";
        case DebugCameraMode::VantageKillcam: return "vantage_killcam";
        case DebugCameraMode::BulletKillcam: return "bullet_killcam";
        case DebugCameraMode::Auto:
        default: return "auto";
    }
}

static DebugCameraMode ActiveDebugCameraMode() {
    return g_config.debugCameraMode;
}

static bool IsDebugCameraModeActive() {
    return ActiveDebugCameraMode() != DebugCameraMode::Auto;
}

static bool ShouldExposeActiveCameraModeName() {
    return g_config.debugMode || DelayManager::IsDebugMode() || IsDebugCameraModeActive();
}

static const char* ActivePlayerCameraModeName() {
    switch (WorldCameraTracker_GetCurrentCamType()) {
        case 0: return "World Static";
        case 1: return "World Dynamic";
        case 2: return g_currentShotUseFpv ? "Player FPV" : "Player 3PV";
        case -1:
            if (g_committedPreference == CAM_WORLD) return "World";
            return g_currentShotUseFpv ? "Player FPV" : "Player 3PV";
        default:
            return "Unknown";
    }
}

static const char* ActiveFlagCameraModeName() {
    switch (WorldCameraTracker_GetCurrentCamType()) {
        case 0: return "Flag World Static";
        case 1: return "Flag World Dynamic";
        case 2: return g_currentShotUseFpv ? "Flag FPV" : "Flag 3PV";
        case -1: return g_currentShotUseFpv ? "Flag FPV" : "Flag 3PV";
        default: return "Unknown";
    }
}

static bool ShouldDropDirectorEventForDebug(const DirectorEvent& event) {
    (void)event;
    return false;
}

static bool DebugKillEventCanPreemptLock(const PendingDirectorEvent& pending) {
    return IsDebugCameraModeActive()
        && pending.event.type == DirectorEventType::Kill
        && g_committedShotKind != ShotKind::KillCam
        && g_committedShotKind != ShotKind::FlagWatch;
}

static bool DebugModeForcesPlayerCamera() {
    DebugCameraMode mode = ActiveDebugCameraMode();
    return mode == DebugCameraMode::Player3pv
        || mode == DebugCameraMode::Fpv
        || mode == DebugCameraMode::VictimLook3pv
        || mode == DebugCameraMode::VantageKillcam
        || mode == DebugCameraMode::BulletKillcam;
}

static int WorldPreferenceForDebugMode() {
    DebugCameraMode mode = ActiveDebugCameraMode();
    if (DebugModeForcesPlayerCamera()) return 3;
    if (mode == DebugCameraMode::World) return 4;
    if (mode == DebugCameraMode::Drone) return -1;
    return -2;
}

static int WorldPreferenceForRequest(const ShotRequest& request) {
    int debugPreference = WorldPreferenceForDebugMode();
    if (debugPreference != -2) return debugPreference;
    if (request.kind == ShotKind::KillCam) {
        return request.killCamStyle == KillCamStyle::DetachedVantage ? 2 : 0;
    }
    return request.preference == CAM_PLAYER ? 2 : 0;
}

static bool ShouldUseFpvForFollowShot() {
    DebugCameraMode mode = ActiveDebugCameraMode();
    if (mode == DebugCameraMode::Fpv) return true;
    if (mode == DebugCameraMode::Player3pv
        || mode == DebugCameraMode::World
        || mode == DebugCameraMode::Drone
        || mode == DebugCameraMode::VictimLook3pv
        || mode == DebugCameraMode::VantageKillcam
        || mode == DebugCameraMode::BulletKillcam) {
        return false;
    }
    return RollFpvForShot();
}

static const char* ViewNameForRequest(const ShotRequest& request) {
    if (request.kind == ShotKind::Drone) return "drone";
    if (request.kind == ShotKind::KillCam) return "world";
    if (g_currentShotUseFpv) return "fpv";
    if (request.preference == CAM_PLAYER) return "player";
    return "world";
}

static std::string HoldTextForRequest(const ShotRequest& request, DWORD now) {
    if (request.kind == ShotKind::FlagWatch) {
        char buf[64];
        if (g_shotHoldUntil != 0 && g_shotHoldUntil > now) {
            sprintf(buf, "%ds", (int)((g_shotHoldUntil - now) / 1000));
            return std::string(buf);
        }
        return "expired";
    }
    if (g_shotHoldUntil != 0 && g_shotHoldUntil > now) {
        return std::to_string((int)((g_shotHoldUntil - now) / 1000)) + "s";
    }
    return "0s";
}

static void FeedCurrentWatching(const ShotRequest& request, DWORD now) {
    PlayerLabel target = ResolvePlayerLabel(request.targetHandle);
    std::string hold = HoldTextForRequest(request, now);
    CommentatorFeed_Line("CURRENT Watching %s (%s) - %s; view=%s; hold=%s",
                         target.name.c_str(),
                         target.team.c_str(),
                         request.reason ? request.reason : "camera change",
                         ViewNameForRequest(request),
                         hold.c_str());
}

static void FeedNextKill(int killerHandle,
                         int victimHandle,
                         int weaponId,
                         int leadMs,
                         float distance,
                         const char* outcome) {
    PlayerLabel killer = ResolvePlayerLabel(killerHandle);
    PlayerLabel victim = ResolvePlayerLabel(victimHandle);
    const char* weaponName = GetWeaponName(weaponId);
    int leadSeconds = LeadSeconds(leadMs);

    if (leadSeconds <= 0) {
        CommentatorFeed_Line("NEXT now %s kills %s with %s (%.0fm); %s",
                             killer.name.c_str(),
                             victim.name.c_str(),
                             weaponName,
                             distance,
                             outcome);
    } else {
        CommentatorFeed_Line("NEXT ~%ds %s kills %s with %s (%.0fm); %s",
                             leadSeconds,
                             killer.name.c_str(),
                             victim.name.c_str(),
                             weaponName,
                             distance,
                             outcome);
    }
}

static void FeedNextFlagCarry(int carrierHandle, const char* flagName, int leadMs) {
    PlayerLabel carrier = ResolvePlayerLabel(carrierHandle);
    int leadSeconds = LeadSeconds(leadMs);

    if (leadSeconds <= 0) {
        CommentatorFeed_Line("NEXT now %s carries %s; switching to flag watch",
                             carrier.name.c_str(),
                             flagName);
    } else {
        CommentatorFeed_Line("NEXT ~%ds %s carries %s; switching to flag watch",
                             leadSeconds,
                             carrier.name.c_str(),
                             flagName);
    }
}

static void FeedNextFlagLost(const char* flagName) {
    CommentatorFeed_Line("NEXT now %s no longer carried; holding flag view briefly", flagName);
}

static bool CurrentCommittedShotUsable(DWORD now) {
    bool usable = false;

    switch (g_state) {
        case CameraState::FollowPlayer:
            usable = CD_IsUsablePlayerHandle(g_currentTargetHandle);
            break;
        case CameraState::FlagWatch:
            usable = g_currentTargetHandle != 0 && CD_IsUsablePlayerHandle(g_currentTargetHandle);
            break;
        case CameraState::Drone:
            usable = DroneCamera_IsActive() && CD_IsUsablePlayerHandle(g_currentTargetHandle);
            break;
        case CameraState::KillCam:
            usable = g_kcKillerHandle != 0;
            break;
        default:
            usable = false;
            break;
    }

    if (usable) {
        g_currentInvalidSince = 0;
        return true;
    }

    if (g_currentInvalidSince == 0) {
        g_currentInvalidSince = now;
        return true;
    }

    return now - g_currentInvalidSince <= TARGET_INVALID_GRACE_MS;
}

static bool IsCommittedShotLocked(DWORD now) {
    return IsShotHoldActive(now) && CurrentCommittedShotUsable(now);
}

static void ClearCommittedShot(DWORD now, const char* reason) {
    if (DroneCamera_IsActive()) {
        DroneCamera_Deactivate();
    }

    g_state = CameraState::Idle;
    g_committedShotKind = ShotKind::None;
    g_committedPreference = CAM_WORLD;
    g_currentTargetHandle = 0;
    g_currentHoldStart = now;
    g_shotHoldUntil = 0;
    g_currentShotUseFpv = false;
    g_currentInvalidSince = 0;
    g_focusDeathTargetHandle = 0;
    g_focusDeathKillerHandle = 0;
    g_focusDeathTick = 0;
    g_focusDeathHoldUntil = 0;
    g_kcStyle = KillCamStyle::BulletTravel;
    g_kcKillerHandle = 0;
    g_kcVictimHandle = 0;
    g_kcVictimAimPointValid = false;
    ClearFlagKillLook();
    ClearFlagCampingGlimpse();
    WorldCameraTracker_ClearTarget();

    CD_Log("[CameraDirector] Shot cleared: %s\n", reason ? reason : "none");
}

static bool CommitShot(const ShotRequest& request, DWORD now) {
    if (request.kind != ShotKind::Drone && DroneCamera_IsActive()) {
        DroneCamera_Deactivate();
    }

    g_currentInvalidSince = 0;

    switch (request.kind) {
        case ShotKind::FollowPlayer: {
            if (!CD_IsAliveUsablePlayerHandle(request.targetHandle)) return false;

            g_state = CameraState::FollowPlayer;
            g_committedShotKind = ShotKind::FollowPlayer;
            g_committedPreference = request.preference;
            g_currentTargetHandle = request.targetHandle;
            g_currentHoldStart = now;
            g_shotHoldUntil = HoldUntil(now, request.holdSeconds);
            g_currentShotUseFpv = RollFpvForShot();
            g_kcStyle = KillCamStyle::BulletTravel;
            g_kcKillerHandle = 0;
            g_kcVictimHandle = 0;
            g_kcVictimAimPointValid = false;
            ClearFlagKillLook();
            ClearFlagCampingGlimpse();

            SetSpectatorToPlayerId(request.targetHandle);
            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(WorldPreferenceForRequest(request));

            CD_Log("[CameraDirector] Commit follow target=%d pref=%s hold=%ds fpv=%s reason=%s\n",
                   request.targetHandle,
                   BudgetName(request.preference),
                   (int)((g_shotHoldUntil - now) / 1000),
                   g_currentShotUseFpv ? "yes" : "no",
                   request.reason ? request.reason : "none");
            FeedCurrentWatching(request, now);
            return true;
        }

        case ShotKind::KillCam: {
            if (!CD_IsAliveUsablePlayerHandle(request.targetHandle)) return false;

            g_state = CameraState::KillCam;
            g_committedShotKind = ShotKind::KillCam;
            g_committedPreference = CAM_WORLD;
            g_killCamPhase = KillCamPhase::Wait;
            g_kcStyle = request.killCamStyle;
            g_kcKillerHandle = request.targetHandle;
            g_kcVictimHandle = request.victimHandle;
            g_kcVictimAimPointValid = request.hasVictimAimPoint;
            if (g_kcVictimAimPointValid) {
                g_kcVictimAimPoint[0] = request.victimAimPoint[0];
                g_kcVictimAimPoint[1] = request.victimAimPoint[1];
                g_kcVictimAimPoint[2] = request.victimAimPoint[2];
            }
            g_kcStartTick = now;
            g_currentTargetHandle = request.targetHandle;
            g_currentHoldStart = now;
            g_shotHoldUntil = HoldUntil(now, request.holdSeconds);
            g_currentShotUseFpv = false;
            ClearFlagKillLook();
            ClearFlagCampingGlimpse();

            SetSpectatorToPlayerId(request.targetHandle);
            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(WorldPreferenceForRequest(request));

            CD_Log("[CameraDirector] Commit killcam killer=%d victim=%d style=%s frozenVictimAim=%s reason=%s\n",
                   request.targetHandle,
                   request.victimHandle,
                   KillCamStyleName(g_kcStyle),
                   g_kcVictimAimPointValid ? "yes" : "no",
                   request.reason ? request.reason : "none");
            FeedCurrentWatching(request, now);
            return true;
        }

        case ShotKind::FlagWatch: {
            if (request.targetHandle == 0) return false;
            bool targetChanged = (g_state != CameraState::FlagWatch
                || g_currentTargetHandle != request.targetHandle);

            g_state = CameraState::FlagWatch;
            g_committedShotKind = ShotKind::FlagWatch;
            g_committedPreference = CAM_WORLD;
            g_currentTargetHandle = request.targetHandle;
            g_currentHoldStart = now;
            g_shotHoldUntil = HoldUntil(now, request.holdSeconds);
            if (IsFlagScoreHoldActive(now)
                && request.targetHandle == g_flagScoreHoldTarget) {
                ExtendShotHoldTo(g_flagScoreHoldUntil);
            }
            g_currentShotUseFpv = ShouldUseFpvForFollowShot();
            g_kcStyle = KillCamStyle::BulletTravel;
            g_kcKillerHandle = 0;
            g_kcVictimHandle = 0;
            g_kcVictimAimPointValid = false;
            if (g_flagKillLookVictimHandle != request.targetHandle) {
                ClearFlagKillLook();
            }
            if (g_flagCampingGlimpseHandle != request.targetHandle) {
                ClearFlagCampingGlimpse();
            }

            SetSpectatorToPlayerId(request.targetHandle);
            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(0);
            if (targetChanged) {
                g_lastFlagWatchTargetSwitch = now;
            }

            CD_Log("[CameraDirector] Commit flag target=%d hold=%ds fpv=%s reason=%s\n",
                   request.targetHandle,
                   (int)((g_shotHoldUntil - now) / 1000),
                   g_currentShotUseFpv ? "yes" : "no",
                   request.reason ? request.reason : "none");
            FeedCurrentWatching(request, now);
            return true;
        }

        case ShotKind::Drone: {
            if (!CD_IsAliveUsablePlayerHandle(request.targetHandle)) return false;
            if (!IsDroneSuitableForTarget(request.targetHandle)) return false;

            if (DroneCamera_IsActive()) {
                DroneCamera_Deactivate();
            }

            float startPos[3] = { 0.0f, 0.0f, 0.0f };
            void* entity = CD_FindEntityByHandle(request.targetHandle);
            if (entity) {
                CD_GetEntityPos(entity, startPos);
                startPos[2] += 2.0f;
            }

            DroneCamera_Activate(startPos);
            DroneCamera_SetTarget(request.targetHandle);

            g_state = CameraState::Drone;
            g_committedShotKind = ShotKind::Drone;
            g_committedPreference = CAM_DRONE;
            g_currentTargetHandle = request.targetHandle;
            g_currentHoldStart = now;
            g_shotHoldUntil = HoldUntil(now, request.holdSeconds);
            g_currentShotUseFpv = false;
            g_kcStyle = KillCamStyle::BulletTravel;
            g_kcKillerHandle = 0;
            g_kcVictimHandle = 0;
            g_kcVictimAimPointValid = false;
            ClearFlagKillLook();
            ClearFlagCampingGlimpse();

            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(-1);

            CD_Log("[CameraDirector] Commit drone target=%d hold=%ds reason=%s\n",
                   request.targetHandle,
                   (int)((g_shotHoldUntil - now) / 1000),
                   request.reason ? request.reason : "none");
            FeedCurrentWatching(request, now);
            return true;
        }

        default:
            return false;
    }
}

static bool RequestShot(const ShotRequest& request, DWORD now) {
    if (request.kind != ShotKind::FlagWatch
        && g_state == CameraState::FlagWatch
        && IsCurrentFlagCarrierHandle(g_currentTargetHandle)) {
        CD_Log("[CameraDirector] Shot request ignored: active flag run target=%d request=%s target=%d\n",
               g_currentTargetHandle,
               ShotKindName(request.kind),
               request.targetHandle);
        return false;
    }

    if (IsFlagScoreHoldActive(now)
        && (request.kind != ShotKind::FlagWatch
            || request.targetHandle != g_flagScoreHoldTarget)) {
        CD_Log("[CameraDirector] Shot request ignored: flag score hold target=%d remaining=%ds request=%s target=%d\n",
               g_flagScoreHoldTarget,
               (int)((g_flagScoreHoldUntil - now) / 1000),
               ShotKindName(request.kind),
               request.targetHandle);
        return false;
    }

    if (!request.force) {
        if (IsCommittedShotLocked(now)) {
            CD_Log("[CameraDirector] Shot request ignored: locked %s remaining=%ds request=%s\n",
                   ShotKindName(g_committedShotKind),
                   (int)((g_shotHoldUntil - now) / 1000),
                   ShotKindName(request.kind));
            return false;
        }
    }

    return CommitShot(request, now);
}

static bool RequestFollowPlayerShot(int targetHandle,
                                    CamBudgetType preference,
                                    bool force,
                                    const char* reason,
                                    DWORD now) {
    ShotRequest request = {
        ShotKind::FollowPlayer,
        targetHandle,
        0,
        KillCamStyle::BulletTravel,
        preference,
        NormalFollowHoldSeconds(preference),
        force,
        reason
    };
    return RequestShot(request, now);
}

static bool RequestKillCamShot(int killerHandle,
                               int victimHandle,
                               KillCamStyle style,
                               bool force,
                               const char* reason,
                               DWORD now,
                               const float* victimAimPoint = nullptr) {
    ShotRequest request = {
        ShotKind::KillCam,
        killerHandle,
        victimHandle,
        style,
        CAM_WORLD,
        GetKillCamTotalDuration(style),
        force,
        reason
    };
    if (victimAimPoint) {
        request.hasVictimAimPoint = true;
        request.victimAimPoint[0] = victimAimPoint[0];
        request.victimAimPoint[1] = victimAimPoint[1];
        request.victimAimPoint[2] = victimAimPoint[2];
    }
    return RequestShot(request, now);
}

static bool RequestFlagWatchShot(int carrierHandle,
                                 const char* reason,
                                 DWORD now,
                                 bool force = false) {
    if (carrierHandle != 0
        && g_state == CameraState::FlagWatch
        && g_currentTargetHandle == carrierHandle
        && CD_IsAliveUsablePlayerHandle(carrierHandle)) {
        return true;
    }

    ShotRequest request = {
        ShotKind::FlagWatch,
        carrierHandle,
        0,
        KillCamStyle::BulletTravel,
        CAM_WORLD,
        NormalFollowHoldSeconds(CAM_WORLD),
        force,
        reason
    };
    return RequestShot(request, now);
}

static bool EnsureFlagScoreHoldCamera(DWORD now) {
    if (g_flagScoreHoldTarget == 0) return false;

    if (!IsFlagScoreHoldActive(now)) {
        ClearFlagScoreHold(now, "expired");
        return false;
    }

    ClearFlagCampingGlimpse();
    ExtendShotHoldTo(g_flagScoreHoldUntil);

    if (g_state != CameraState::FlagWatch
        || g_currentTargetHandle != g_flagScoreHoldTarget) {
        RequestFlagWatchShot(g_flagScoreHoldTarget,
                             "flag score hold enforce",
                             now,
                             true);
        ExtendShotHoldTo(g_flagScoreHoldUntil);
    }

    return true;
}

static bool RequestFlagCampingGlimpse(int carrierHandle, DWORD now) {
    FlagCarrierMotion* motion = FlagMotionForHandle(carrierHandle);
    if (!motion || !motion->camping) return false;

    g_flagCampingGlimpseHandle = carrierHandle;
    g_flagCampingGlimpseUntil = HoldUntil(now, g_config.flagCampingGlimpseDuration);
    motion->lastGlimpseTick = now;

    if (!RequestFlagWatchShot(carrierHandle, "flag camper glimpse", now)) {
        ClearFlagCampingGlimpse();
        return false;
    }

    g_shotHoldUntil = g_flagCampingGlimpseUntil;
    return true;
}

static bool TryStartFlagCampingGlimpse(DWORD now) {
    if (g_state == CameraState::KillCam) return false;
    if (g_state == CameraState::FlagWatch) return false;
    if (PickMovingFlagCarrierTarget() != 0) return false;

    float interval = g_config.flagCampingGlimpseInterval;
    if (interval < g_config.flagCampingGlimpseDuration + 1.0f) {
        interval = g_config.flagCampingGlimpseDuration + 1.0f;
    }
    DWORD intervalMs = (DWORD)(interval * 1000.0f);

    FlagCarrierMotion* motions[2] = { &g_flagMotionUS, &g_flagMotionVC };
    for (FlagCarrierMotion* motion : motions) {
        if (!motion->initialized || !motion->camping) continue;
        if (!CD_IsAliveUsablePlayerHandle(motion->handle)) continue;
        if (now - motion->lastGlimpseTick < intervalMs) continue;

        CD_Log("[CameraDirector] Showing camping flag carrier briefly handle=%d\n",
               motion->handle);
        return RequestFlagCampingGlimpse(motion->handle, now);
    }

    return false;
}

static bool RequestDroneShot(int targetHandle,
                             bool force,
                             const char* reason,
                             DWORD now) {
    ShotRequest request = {
        ShotKind::Drone,
        targetHandle,
        0,
        KillCamStyle::BulletTravel,
        CAM_DRONE,
        g_config.camMinHoldDrone,
        force,
        reason
    };
    return RequestShot(request, now);
}

static void CompleteKillCam(DWORD now) {
    int nextTarget = g_kcKillerHandle;
    g_kcKillerHandle = 0;
    g_kcVictimHandle = 0;
    g_kcVictimAimPointValid = false;
    g_lastEventTick = now;
    g_shotHoldUntil = 0;

    CD_Log("[CameraDirector] KillCam ended\n");

    if (CD_IsAliveUsablePlayerHandle(nextTarget)) {
        RequestFollowPlayerShot(nextTarget,
                                DebugModeForcesPlayerCamera() ? CAM_PLAYER : CAM_WORLD,
                                true,
                                "killcam complete",
                                now);
    } else {
        ClearCommittedShot(now, "killcam target lost");
    }
}

static float ValidShare(float value) {
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

// Compute deficit for each camera type and return the one with highest deficit
static CamBudgetType GetHighestDeficitType() {
    float playerShare = ValidShare(g_config.camSharePlayer);
    float worldShare = ValidShare(g_config.camShareWorld);
    float droneShare = ValidShare(g_config.camShareDrone);

    if (droneShare > 0.0f && !IsDroneSuitable()) {
        droneShare = 0.0f;
    }

    float shareTotal = playerShare + worldShare + droneShare;
    if (shareTotal <= 0.001f) {
        return CAM_WORLD;
    }

    playerShare /= shareTotal;
    worldShare /= shareTotal;
    droneShare /= shareTotal;

    float totalTime = g_playerCamTime + g_worldCamTime + g_droneCamTime;
    if (totalTime <= 1.0f) {
        float roll = (float)(rand() % 10000) / 10000.0f;
        if (roll < playerShare) return CAM_PLAYER;
        if (roll < playerShare + worldShare) return CAM_WORLD;
        return CAM_DRONE;
    }

    float playerDeficit = playerShare * totalTime - g_playerCamTime;
    float worldDeficit = worldShare * totalTime - g_worldCamTime;
    float droneDeficit = droneShare > 0.0f
        ? droneShare * totalTime - g_droneCamTime
        : -1.0e30f;

    if (playerDeficit >= worldDeficit && playerDeficit >= droneDeficit) {
        return CAM_PLAYER;
    }
    if (worldDeficit >= droneDeficit) {
        return CAM_WORLD;
    }
    return CAM_DRONE;
}

static bool StartNextScheduledShot(DWORD now, const char* reason) {
    int currentTarget = CD_IsAliveUsablePlayerHandle(g_currentTargetHandle)
        && !IsCampingFlagCarrier(g_currentTargetHandle)
        ? g_currentTargetHandle
        : 0;

    DebugCameraMode debugMode = ActiveDebugCameraMode();
    if (debugMode == DebugCameraMode::Drone) {
        int droneTarget = currentTarget != 0 ? currentTarget : PickRandomActivePlayer(0);
        return droneTarget != 0
            && IsDroneSuitableForTarget(droneTarget)
            && RequestDroneShot(droneTarget, false, reason, now);
    }

    CamBudgetType needed = GetHighestDeficitType();
    if (debugMode == DebugCameraMode::World) {
        needed = CAM_WORLD;
    } else if (IsDebugCameraModeActive()) {
        needed = CAM_PLAYER;
    }

    if (needed == CAM_DRONE) {
        int droneTarget = currentTarget != 0 ? currentTarget : PickRandomActivePlayer(0);
        if (droneTarget != 0 && IsDroneSuitableForTarget(droneTarget)) {
            return RequestDroneShot(droneTarget, false, reason, now);
        }
        needed = CAM_WORLD;
    }

    int picked = PickRandomActivePlayer(g_currentTargetHandle);
    if (picked == 0) picked = currentTarget;
    if (picked == 0) {
        ClearCommittedShot(now, "no active players");
        return false;
    }

    CamBudgetType preference = (needed == CAM_PLAYER) ? CAM_PLAYER : CAM_WORLD;
    return RequestFollowPlayerShot(picked, preference, false, reason, now);
}

static void UpdateKillCamPhase() {
    float elapsed = GetKillCamElapsedInternal();
    float waitEnd = GetKillCamWaitDuration(g_kcStyle);
    float transEnd = waitEnd + GetKillCamTransitionDuration(g_kcStyle);
    float totalEnd = GetKillCamTotalDuration();

    if (elapsed < waitEnd) {
        g_killCamPhase = KillCamPhase::Wait;
    } else if (elapsed < transEnd) {
        g_killCamPhase = KillCamPhase::Transition;
    } else if (elapsed < totalEnd) {
        g_killCamPhase = KillCamPhase::Attached;
    } else {
        CompleteKillCam(GetTickCount());
    }
}

// ============================================================================
// Server event planning
// ============================================================================

static DWORD DirectorPreRollMsForEvent(const DirectorEvent& event) {
    // Starting flag-lost/capture events early makes the camera abandon the
    // carrier before the spectator-visible score. Flag pickup events still
    // use pre-roll so the view can catch the start of a run.
    if (event.type == DirectorEventType::Flag
        && event.usCarrier == 0
        && event.vcCarrier == 0) {
        return 0;
    }

    float preRollSeconds = g_config.directorPreRollSeconds;
    if (!std::isfinite(preRollSeconds) || preRollSeconds < 0.0f) {
        preRollSeconds = 5.0f;
    }

    DWORD preRollMs = (DWORD)(preRollSeconds * 1000.0f + 0.5f);
    DWORD availableLead = event.availableLeadMs > 0
        ? (DWORD)event.availableLeadMs
        : 0;

    if (preRollMs > availableLead) {
        preRollMs = availableLead;
    }
    return preRollMs;
}

static DWORD PlannedStartTickForEvent(const DirectorEvent& event) {
    DWORD preRollMs = DirectorPreRollMsForEvent(event);
    DWORD plannedStart = event.visibleTick >= preRollMs
        ? event.visibleTick - preRollMs
        : event.rawTick;

    if (!TickReached(plannedStart, event.rawTick)) {
        plannedStart = event.rawTick;
    }

    return plannedStart;
}

static bool IsEventStale(const DirectorEvent& event, DWORD now) {
    DWORD grace = event.type == DirectorEventType::Flag
        ? FLAG_EVENT_STALE_GRACE_MS
        : KILL_EVENT_STALE_GRACE_MS;
    return TickReached(now, event.visibleTick + grace);
}

static int EventScore(const DirectorEvent& event) {
    if (event.type == DirectorEventType::Flag) {
        return 300;
    }
    return IsDebugCameraModeActive() ? 200 : 100;
}

static void AttachEventSnapshots(DirectorEvent& event) {
    if (event.type == DirectorEventType::Kill) {
        if (!event.hasKillerSnapshot) {
            event.hasKillerSnapshot = ServerStateSniffer_GetSnapshotAt(
                event.killerHandle,
                event.visibleTick,
                &event.killerSnapshot);
        }
        if (!event.hasVictimSnapshot) {
            event.hasVictimSnapshot = ServerStateSniffer_GetSnapshotAt(
                event.victimHandle,
                event.visibleTick,
                &event.victimSnapshot);
        }
    } else {
        if (!event.hasUSCarrierSnapshot) {
            event.hasUSCarrierSnapshot = ServerStateSniffer_GetSnapshotAt(
                event.usCarrier,
                event.visibleTick,
                &event.usCarrierSnapshot);
        }
        if (!event.hasVCCarrierSnapshot) {
            event.hasVCCarrierSnapshot = ServerStateSniffer_GetSnapshotAt(
                event.vcCarrier,
                event.visibleTick,
                &event.vcCarrierSnapshot);
        }
    }
}

static void ApplyFlagTelemetryToEvent(DirectorEvent& event, const ServerTelemetryFlagEvent& telemetry) {
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

static unsigned int InferFlagActionForEvent(const DirectorEvent& event, int* playerHandle) {
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
    return event.flagAction;
}

static void EnrichFlagEventForPlanning(DirectorEvent& event) {
    if (event.type != DirectorEventType::Flag || event.hasFlagTelemetry) return;

    int playerHandle = event.flagPlayerHandle;
    unsigned int action = event.flagAction;
    if (action == 0) {
        action = InferFlagActionForEvent(event, &playerHandle);
    }

    ServerTelemetryFlagEvent telemetry = {};
    if (ServerTelemetry_TryFindFlag(playerHandle, action, event.rawTick, &telemetry) ||
        ServerTelemetry_TryFindFlag(0, action, event.rawTick, &telemetry) ||
        ServerTelemetry_TryFindFlag(0, 0, event.rawTick, &telemetry)) {
        ApplyFlagTelemetryToEvent(event, telemetry);
    } else {
        event.flagAction = action;
        event.flagPlayerHandle = playerHandle;
    }
}

static bool ShouldSupersedePendingFlagEvent(const DirectorEvent& pending, const DirectorEvent& incoming) {
    if (pending.type != DirectorEventType::Flag || incoming.type != DirectorEventType::Flag) {
        return false;
    }

    bool incomingGenericZeroCarrier = !incoming.hasFlagTelemetry
        && incoming.usCarrier == 0
        && incoming.vcCarrier == 0;
    bool pendingSemanticTake = pending.hasFlagTelemetry && pending.flagAction == 1;
    if (incomingGenericZeroCarrier && pendingSemanticTake) {
        return false;
    }

    if (incoming.hasFlagTelemetry && pending.hasFlagTelemetry) {
        return incoming.flagAction >= pending.flagAction
            || (incoming.flagReasonFlags & STREAMER_FLAG_REASON_SCORE) != 0;
    }

    return incoming.hasFlagTelemetry || !pending.hasFlagTelemetry;
}

static void QueueDirectorEvent(DirectorEvent event, const char* reason) {
    EnrichFlagEventForPlanning(event);
    AttachEventSnapshots(event);
    if (event.type == DirectorEventType::Kill) {
        PlayerLifeTracker_OnKill(event);
    }

    if (ShouldDropDirectorEventForDebug(event)) {
        DWORD plannedStart = PlannedStartTickForEvent(event);
        TimingLog("[Plan] seq=%llu type=%s raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=0 lead=%d reason=drop-debug-%s\n",
                  event.sequence,
                  DirectorEventTypeName(event.type),
                  event.rawTick,
                  event.playbackDelayMs,
                  event.visibleTick,
                  plannedStart,
                  event.availableLeadMs,
                  DebugCameraModeName(ActiveDebugCameraMode()));
        return;
    }

    if (event.type == DirectorEventType::Flag) {
        for (size_t i = 0; i < g_eventQueue.size();) {
            if (ShouldSupersedePendingFlagEvent(g_eventQueue[i].event, event)) {
                TimingLog("[Plan] seq=%llu type=flag raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=0 lead=%d reason=drop-superseded-by-flag-%llu\n",
                          g_eventQueue[i].event.sequence,
                          g_eventQueue[i].event.rawTick,
                          g_eventQueue[i].event.playbackDelayMs,
                          g_eventQueue[i].event.visibleTick,
                          g_eventQueue[i].plannedStartTick,
                          g_eventQueue[i].event.availableLeadMs,
                          event.sequence);
                g_eventQueue.erase(g_eventQueue.begin() + i);
            } else {
                ++i;
            }
        }
    }

    if (g_eventQueue.size() >= MAX_DIRECTOR_EVENT_QUEUE) {
        TimingLog("[Plan] seq=%llu type=%s raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=0 lead=%d reason=drop-director-queue-full\n",
                  g_eventQueue.front().event.sequence,
                  DirectorEventTypeName(g_eventQueue.front().event.type),
                  g_eventQueue.front().event.rawTick,
                  g_eventQueue.front().event.playbackDelayMs,
                  g_eventQueue.front().event.visibleTick,
                  g_eventQueue.front().plannedStartTick,
                  g_eventQueue.front().event.availableLeadMs);
        g_eventQueue.erase(g_eventQueue.begin());
    }

    PendingDirectorEvent pending = {};
    pending.event = event;
    pending.plannedStartTick = PlannedStartTickForEvent(event);
    pending.dueLogged = false;
    g_eventQueue.push_back(pending);

    TimingLog("[Plan] seq=%llu type=%s raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=0 lead=%d reason=%s\n",
              event.sequence,
              DirectorEventTypeName(event.type),
              event.rawTick,
              event.playbackDelayMs,
              event.visibleTick,
              pending.plannedStartTick,
              event.availableLeadMs,
              reason ? reason : "queued");
}

static void DrainSnifferEvents() {
    std::vector<DirectorEvent> events;
    ServerStateSniffer_DrainEvents(events);

    for (size_t i = 0; i < events.size(); ++i) {
        QueueDirectorEvent(events[i], "queued-from-sniffer");
    }
}

static bool ApplyVictimLookForEvent(const DirectorEvent& event, DWORD now) {
    if (event.type != DirectorEventType::Kill) return false;
    if (IsDebugCameraModeActive()) return false;

    float frozenKillerAimPoint[3] = {};
    bool hasFrozenKillerAimPoint = GetFrozenKillerAimPoint(event, frozenKillerAimPoint);

    if (!MaybeStartVictimKillLook(event.killerHandle,
                                  event.victimHandle,
                                  now,
                                  event.visibleTick,
                                  hasFrozenKillerAimPoint ? frozenKillerAimPoint : nullptr)) {
        return false;
    }

    if ((g_state == CameraState::FlagWatch || g_state == CameraState::FollowPlayer)
        && (g_shotHoldUntil == 0 || TickReached(event.visibleTick, g_shotHoldUntil))) {
        g_shotHoldUntil = event.visibleTick;
    }

    CD_Log("[CameraDirector] Kill preserved: victim death look target=%d killer=%d seq=%llu\n",
           event.victimHandle,
           event.killerHandle,
           event.sequence);
    return true;
}

static bool CommitKillEvent(const DirectorEvent& event, DWORD now) {
    g_lastEventTick = now;
    UpdatePlayerActivity(now);
    BumpPlayerActivity(event.killerHandle, 0.8f);
    BumpPlayerActivity(event.victimHandle, 0.4f);

    bool usedLiveAheadPosition = false;
    float distance = GetKillDistanceFromEvent(event, &usedLiveAheadPosition);
    float frozenVictimAimPoint[3] = {};
    bool hasFrozenVictimAimPoint = GetFrozenVictimAimPoint(event, frozenVictimAimPoint);

    DebugCameraMode debugMode = ActiveDebugCameraMode();
    if (debugMode != DebugCameraMode::Auto) {
        bool committed = false;
        switch (debugMode) {
            case DebugCameraMode::VictimLook3pv:
                committed = RequestKillCamShot(event.killerHandle,
                                               event.victimHandle,
                                               KillCamStyle::DetachedVantage,
                                               true,
                                               "debug victim-look 3pv",
                                               now,
                                               hasFrozenVictimAimPoint ? frozenVictimAimPoint : nullptr);
                break;
            case DebugCameraMode::VantageKillcam:
                committed = RequestKillCamShot(event.killerHandle,
                                               event.victimHandle,
                                               KillCamStyle::DetachedVantage,
                                               true,
                                               "debug vantage killcam",
                                               now,
                                               hasFrozenVictimAimPoint ? frozenVictimAimPoint : nullptr);
                break;
            case DebugCameraMode::BulletKillcam:
                committed = RequestKillCamShot(event.killerHandle,
                                               event.victimHandle,
                                               KillCamStyle::BulletTravel,
                                               true,
                                               "debug bullet killcam",
                                               now,
                                               nullptr);
                break;
            case DebugCameraMode::Drone:
                committed = RequestDroneShot(event.killerHandle,
                                             true,
                                             "debug drone",
                                             now);
                break;
            case DebugCameraMode::World:
                committed = RequestFollowPlayerShot(event.killerHandle,
                                                    CAM_WORLD,
                                                    true,
                                                    "debug world",
                                                    now);
                break;
            case DebugCameraMode::Fpv:
            case DebugCameraMode::Player3pv:
            default:
                committed = RequestFollowPlayerShot(event.killerHandle,
                                                    CAM_PLAYER,
                                                    true,
                                                    "debug player",
                                                    now);
                break;
        }

        if (committed) {
            g_lastKillSwitch = now;
            FeedNextKill(event.killerHandle,
                         event.victimHandle,
                         event.weaponId,
                         event.availableLeadMs,
                         distance,
                         DebugCameraModeName(debugMode));
            CommentaryEngine_OnCommittedDirectorEvent(event,
                                                      now,
                                                      g_currentTargetHandle,
                                                      ShotKindName(g_committedShotKind));
        }
        return committed;
    }

    if (ApplyVictimLookForEvent(event, now)) {
        FeedNextKill(event.killerHandle,
                     event.victimHandle,
                     event.weaponId,
                     event.availableLeadMs,
                     distance,
                     "victim-look");
        CommentaryEngine_OnCommittedDirectorEvent(event,
                                                  now,
                                                  g_currentTargetHandle,
                                                  ShotKindName(g_committedShotKind));
        return true;
    }

    KillCamStyle style = (distance >= g_config.killCamLongRangeDistance)
        ? KillCamStyle::BulletTravel
        : KillCamStyle::DetachedVantage;

    float chance = (style == KillCamStyle::BulletTravel)
        ? g_config.bulletKillCamChance
        : g_config.detachedKillCamChance;
    bool cinematic = RollCinematic(chance);

    CD_Log("[CameraDirector] Kill plan commit: seq=%llu killer=%d victim=%d weapon=%d distance=%.2fm liveAhead=%s style=%s chance=%d%% result=%s\n",
           event.sequence,
           event.killerHandle,
           event.victimHandle,
           event.weaponId,
           distance,
           usedLiveAheadPosition ? "yes" : "no",
           KillCamStyleName(style),
           (int)(chance * 100),
           cinematic ? "killcam" : "follow");

    bool committed = false;
    if (cinematic) {
        committed = RequestKillCamShot(event.killerHandle,
                                       event.victimHandle,
                                       style,
                                       false,
                                       "queued kill",
                                       now,
                                       (style == KillCamStyle::DetachedVantage && hasFrozenVictimAimPoint)
                                           ? frozenVictimAimPoint
                                           : nullptr);
    } else if (CD_IsAliveUsablePlayerHandle(event.killerHandle)) {
        committed = RequestFollowPlayerShot(event.killerHandle,
                                            CAM_WORLD,
                                            false,
                                            "queued kill follow",
                                            now);
    }

    if (committed) {
        g_lastKillSwitch = now;
        FeedNextKill(event.killerHandle,
                     event.victimHandle,
                     event.weaponId,
                     event.availableLeadMs,
                     distance,
                     cinematic ? "killcam" : "follow");
        CommentaryEngine_OnCommittedDirectorEvent(event,
                                                  now,
                                                  g_currentTargetHandle,
                                                  ShotKindName(g_committedShotKind));
    }

    return committed;
}

static int PickFlagTargetForEvent(const DirectorEvent& event, bool wasDualFlag) {
    int usCarrier = event.usCarrier;
    int vcCarrier = event.vcCarrier;

    if (usCarrier != 0 && vcCarrier == 0) return usCarrier;
    if (vcCarrier != 0 && usCarrier == 0) return vcCarrier;
    if (usCarrier == 0 && vcCarrier == 0) return 0;

    if (IsUsableActiveFlagCarrier(g_currentTargetHandle)) {
        return g_currentTargetHandle;
    }
    if (IsUsableActiveFlagCarrier(usCarrier)) {
        return usCarrier;
    }
    if (IsUsableActiveFlagCarrier(vcCarrier)) {
        return vcCarrier;
    }
    return wasDualFlag ? g_currentTargetHandle : usCarrier;
}

static bool CommitFlagEvent(const DirectorEvent& event, DWORD now) {
    int previousUSCarrier = g_flagCarrierUS;
    int previousVCCarrier = g_flagCarrierVC;
    int previousWatchedCarrier = g_currentTargetHandle;
    bool wasDualFlag = (g_flagCarrierUS != 0 && g_flagCarrierVC != 0);

    g_flagCarrierUS = event.usCarrier;
    g_flagCarrierVC = event.vcCarrier;
    g_lastEventTick = now;
    UpdatePlayerActivity(now);
    BumpPlayerActivity(event.usCarrier, 0.6f);
    BumpPlayerActivity(event.vcCarrier, 0.6f);
    UpdateFlagCarrierMotion(now);

    bool anyFlag = (event.usCarrier != 0 || event.vcCarrier != 0);
    bool dualFlag = (event.usCarrier != 0 && event.vcCarrier != 0);

    if (anyFlag) {
        ClearFlagScoreHold(now, "new flag carrier");
        g_flagLostGraceActive = false;
        int target = PickFlagTargetForEvent(event, wasDualFlag);

        if (dualFlag && !wasDualFlag) {
            g_flagAlternateTimer = now;
            g_flagAlternateDelay = RandomRange(FLAG_ALTERNATE_MIN_SEC, FLAG_ALTERNATE_MAX_SEC);
        }

        bool committed = target != 0
            && RequestFlagWatchShot(target,
                                    dualFlag ? "queued dual flag" : "queued flag",
                                    now,
                                    true);
        if (committed) {
            FeedNextFlagCarry(target,
                              FlagNameForCarrier(target, event.usCarrier, event.vcCarrier),
                              event.availableLeadMs);
            CommentaryEngine_OnCommittedDirectorEvent(event,
                                                      now,
                                                      g_currentTargetHandle,
                                                      ShotKindName(g_committedShotKind));
        }
        return committed;
    }

    int scoreHoldTarget = 0;
    if (previousWatchedCarrier != 0
        && (previousWatchedCarrier == previousUSCarrier
            || previousWatchedCarrier == previousVCCarrier)) {
        scoreHoldTarget = previousWatchedCarrier;
    } else if (previousUSCarrier != 0) {
        scoreHoldTarget = previousUSCarrier;
    } else if (previousVCCarrier != 0) {
        scoreHoldTarget = previousVCCarrier;
    }

    if (scoreHoldTarget != 0) {
        DWORD scoreHoldUntil = event.visibleTick + FLAG_SCORE_POST_HOLD_MS;
        if (TickReached(now, scoreHoldUntil)) {
            scoreHoldUntil = now + FLAG_SCORE_POST_HOLD_MS;
        }

        g_flagScoreHoldTarget = scoreHoldTarget;
        g_flagScoreHoldUntil = scoreHoldUntil;
        g_flagLostGraceActive = true;
        g_flagLostTimestamp = now;
        ClearFlagCampingGlimpse();

        bool scoreHoldCommitted = RequestFlagWatchShot(scoreHoldTarget, "flag score hold", now, true);
        ExtendShotHoldTo(g_flagScoreHoldUntil);

        CD_Log("[CameraDirector] Flag score hold target=%d until=%lu post=%dms visible=%lu now=%lu previousUS=%d previousVC=%d watched=%d\n",
               g_flagScoreHoldTarget,
               g_flagScoreHoldUntil,
               (int)FLAG_SCORE_POST_HOLD_MS,
               event.visibleTick,
               now,
               previousUSCarrier,
               previousVCCarrier,
               previousWatchedCarrier);

        const char* lostFlag = "flag";
        if (previousUSCarrier != 0 && previousVCCarrier != 0) {
            lostFlag = "US and VC flags";
        } else if (previousUSCarrier != 0) {
            lostFlag = "US flag";
        } else if (previousVCCarrier != 0) {
            lostFlag = "VC flag";
        }
        FeedNextFlagLost(lostFlag);
        if (scoreHoldCommitted) {
            CommentaryEngine_OnCommittedDirectorEvent(event,
                                                      now,
                                                      g_currentTargetHandle,
                                                      ShotKindName(g_committedShotKind));
        }
    }

    if (g_state == CameraState::FlagWatch && !g_flagLostGraceActive) {
        g_flagLostGraceActive = true;
        g_flagLostTimestamp = now;
        CD_Log("[CameraDirector] Flag lost queued - grace period started\n");

        const char* lostFlag = "flag";
        if (previousUSCarrier != 0 && previousVCCarrier != 0) {
            lostFlag = "US and VC flags";
        } else if (previousUSCarrier != 0) {
            lostFlag = "US flag";
        } else if (previousVCCarrier != 0) {
            lostFlag = "VC flag";
        }
        FeedNextFlagLost(lostFlag);
        return true;
    }

    return true;
}

static bool CommitDirectorEvent(const DirectorEvent& event, DWORD now) {
    if (event.type == DirectorEventType::Kill) {
        return CommitKillEvent(event, now);
    }
    return CommitFlagEvent(event, now);
}

static bool TryApplyQueuedVictimLook(DWORD now) {
    for (size_t i = 0; i < g_eventQueue.size(); ++i) {
        PendingDirectorEvent& pending = g_eventQueue[i];
        if (pending.event.type != DirectorEventType::Kill) continue;
        if (!TickReached(now, pending.plannedStartTick)) continue;
        if (g_currentTargetHandle != pending.event.victimHandle) continue;

        if (ApplyVictimLookForEvent(pending.event, now)) {
            TimingLog("[Plan] seq=%llu type=kill raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=%lu lead=%d reason=commit-victim-look-locked-shot\n",
                      pending.event.sequence,
                      pending.event.rawTick,
                      pending.event.playbackDelayMs,
                      pending.event.visibleTick,
                      pending.plannedStartTick,
                      now,
                      pending.event.availableLeadMs);
            g_eventQueue.erase(g_eventQueue.begin() + i);
            return true;
        }
    }

    return false;
}

static bool PromoteQueuedEvents(DWORD now) {
    bool changed = false;

    for (;;) {
        for (size_t i = 0; i < g_eventQueue.size();) {
            if (IsEventStale(g_eventQueue[i].event, now)) {
                TimingLog("[Plan] seq=%llu type=%s raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=0 lead=%d reason=drop-stale\n",
                          g_eventQueue[i].event.sequence,
                          DirectorEventTypeName(g_eventQueue[i].event.type),
                          g_eventQueue[i].event.rawTick,
                          g_eventQueue[i].event.playbackDelayMs,
                          g_eventQueue[i].event.visibleTick,
                          g_eventQueue[i].plannedStartTick,
                          g_eventQueue[i].event.availableLeadMs);
                CD_Log("[CameraDirector] Stale event dropped seq=%llu type=%s\n",
                       g_eventQueue[i].event.sequence,
                       DirectorEventTypeName(g_eventQueue[i].event.type));
                g_eventQueue.erase(g_eventQueue.begin() + i);
                changed = true;
            } else {
                ++i;
            }
        }

        if (IsFlagScoreHoldActive(now)) {
            return changed;
        }

        int bestIndex = -1;
        int bestScore = -1;
        for (size_t i = 0; i < g_eventQueue.size(); ++i) {
            PendingDirectorEvent& pending = g_eventQueue[i];
            if (!TickReached(now, pending.plannedStartTick)) continue;

            int score = EventScore(pending.event);
            if (bestIndex < 0 || score > bestScore) {
                bestIndex = (int)i;
                bestScore = score;
            }
        }

        if (bestIndex < 0) {
            return changed;
        }

        bool debugPreempt = DebugKillEventCanPreemptLock(g_eventQueue[bestIndex]);
        if (IsCommittedShotLocked(now) && !debugPreempt) {
            bool appliedVictimLook = TryApplyQueuedVictimLook(now);
            if (appliedVictimLook) {
                return true;
            }

            PendingDirectorEvent& pending = g_eventQueue[bestIndex];
            if (!pending.dueLogged) {
                TimingLog("[Plan] seq=%llu type=%s raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=0 lead=%d reason=waiting-for-lock-%s\n",
                          pending.event.sequence,
                          DirectorEventTypeName(pending.event.type),
                          pending.event.rawTick,
                          pending.event.playbackDelayMs,
                          pending.event.visibleTick,
                          pending.plannedStartTick,
                          pending.event.availableLeadMs,
                          ShotKindName(g_committedShotKind));
                CD_Log("[CameraDirector] Event queued behind locked shot seq=%llu type=%s lockRemaining=%ds\n",
                       pending.event.sequence,
                       DirectorEventTypeName(pending.event.type),
                       (int)((g_shotHoldUntil - now) / 1000));
                pending.dueLogged = true;
            }
            return changed;
        }
        if (debugPreempt) {
            CD_Log("[CameraDirector] Debug kill preempting locked %s seq=%llu\n",
                   ShotKindName(g_committedShotKind),
                   g_eventQueue[bestIndex].event.sequence);
        }

        PendingDirectorEvent pending = g_eventQueue[bestIndex];

        if (!IsDebugCameraModeActive()
            && pending.event.type == DirectorEventType::Kill
            && g_state == CameraState::FlagWatch
            && g_currentTargetHandle == pending.event.victimHandle
            && IsCurrentFlagCarrierHandle(pending.event.victimHandle)
            && !TickReached(now, pending.event.visibleTick)) {
            if (ApplyVictimLookForEvent(pending.event, now)) {
                TimingLog("[Plan] seq=%llu type=kill raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=%lu lead=%d reason=commit-victim-look-current-shot\n",
                          pending.event.sequence,
                          pending.event.rawTick,
                          pending.event.playbackDelayMs,
                          pending.event.visibleTick,
                          pending.plannedStartTick,
                          now,
                          pending.event.availableLeadMs);
                g_eventQueue.erase(g_eventQueue.begin() + bestIndex);
                return true;
            }

            if (g_shotHoldUntil == 0 || TickReached(pending.event.visibleTick, g_shotHoldUntil)) {
                g_shotHoldUntil = pending.event.visibleTick;
            }
            if (!pending.dueLogged) {
                TimingLog("[Plan] seq=%llu type=kill raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=0 lead=%d reason=waiting-for-victim-look-start\n",
                          pending.event.sequence,
                          pending.event.rawTick,
                          pending.event.playbackDelayMs,
                          pending.event.visibleTick,
                          pending.plannedStartTick,
                          pending.event.availableLeadMs);
                g_eventQueue[bestIndex].dueLogged = true;
            }
            return changed;
        }

        g_eventQueue.erase(g_eventQueue.begin() + bestIndex);

        bool committed = CommitDirectorEvent(pending.event, now);
        TimingLog("[Plan] seq=%llu type=%s raw=%lu playbackDelay=%d visible=%lu plannedStart=%lu actualCommit=%lu lead=%d reason=%s\n",
                  pending.event.sequence,
                  DirectorEventTypeName(pending.event.type),
                  pending.event.rawTick,
                  pending.event.playbackDelayMs,
                  pending.event.visibleTick,
                  pending.plannedStartTick,
                  now,
                  pending.event.availableLeadMs,
                  committed ? "commit" : "drop-target-invalid");

        if (committed) {
            CD_Log("[CameraDirector] Queued event promoted seq=%llu type=%s\n",
                   pending.event.sequence,
                   DirectorEventTypeName(pending.event.type));
            return true;
        }

        changed = true;
    }
}

static bool RetargetAfterFocusDeath(DWORD now) {
    int deadTarget = g_focusDeathTargetHandle;
    int killer = g_focusDeathKillerHandle;

    g_focusDeathTargetHandle = 0;
    g_focusDeathKillerHandle = 0;
    g_focusDeathTick = 0;
    g_focusDeathHoldUntil = 0;

    ClearCommittedShot(now, "focus death hold complete");

    if (PromoteQueuedEvents(now)) {
        TimingLog("[Director] focus-death-retarget dead=%d reason=pending-event now=%lu\n",
                  deadTarget,
                  now);
        return true;
    }

    if (killer != 0
        && killer != deadTarget
        && CD_IsAliveUsablePlayerHandle(killer)) {
        bool committed = RequestFollowPlayerShot(killer,
                                                 DebugModeForcesPlayerCamera() ? CAM_PLAYER : CAM_WORLD,
                                                 true,
                                                 "focus death killer",
                                                 now);
        TimingLog("[Director] focus-death-retarget dead=%d target=%d reason=killer committed=%d now=%lu\n",
                  deadTarget,
                  killer,
                  committed ? 1 : 0,
                  now);
        if (committed) return true;
    }

    int flagTarget = PickMovingFlagCarrierTarget();
    if (flagTarget != 0
        && flagTarget != deadTarget
        && RequestFlagWatchShot(flagTarget, "focus death flag carrier", now, true)) {
        TimingLog("[Director] focus-death-retarget dead=%d target=%d reason=flag now=%lu\n",
                  deadTarget,
                  flagTarget,
                  now);
        return true;
    }

    int picked = PickRandomActivePlayer(deadTarget);
    if (picked != 0
        && RequestFollowPlayerShot(picked,
                                   DebugModeForcesPlayerCamera() ? CAM_PLAYER : CAM_WORLD,
                                   true,
                                   "focus death random alive",
                                   now)) {
        TimingLog("[Director] focus-death-retarget dead=%d target=%d reason=random-alive now=%lu\n",
                  deadTarget,
                  picked,
                  now);
        return true;
    }

    TimingLog("[Director] focus-death-retarget dead=%d reason=no-target now=%lu\n",
              deadTarget,
              now);
    return false;
}

static bool HandleCurrentFocusDeath(DWORD now) {
    if (g_currentTargetHandle == 0) return false;
    if (g_state != CameraState::FollowPlayer
        && g_state != CameraState::FlagWatch
        && g_state != CameraState::Drone) {
        return false;
    }

    PlayerLifeState life = PlayerLifeTracker_Get(g_currentTargetHandle);
    if (life.status != PlayerLifeStatus::RecentlyDied
        || life.deathTick == 0
        || !TickReached(now, life.deathTick)) {
        if (g_focusDeathTargetHandle == g_currentTargetHandle
            && PlayerLifeTracker_IsAlive(g_currentTargetHandle)) {
            g_focusDeathTargetHandle = 0;
            g_focusDeathKillerHandle = 0;
            g_focusDeathTick = 0;
            g_focusDeathHoldUntil = 0;
        }
        return false;
    }

    if (g_focusDeathTargetHandle != g_currentTargetHandle
        || g_focusDeathTick != life.deathTick) {
        g_focusDeathTargetHandle = g_currentTargetHandle;
        g_focusDeathKillerHandle = life.killerHandle;
        g_focusDeathTick = life.deathTick;
        g_focusDeathHoldUntil = life.deathTick + FOCUS_DEATH_HOLD_MS;
        if (TickReached(now, g_focusDeathHoldUntil)) {
            g_focusDeathHoldUntil = now + 700;
        }
        ExtendShotHoldTo(g_focusDeathHoldUntil);

        CommentaryEngine_OnFocusDeath(g_focusDeathTargetHandle,
                                      g_focusDeathKillerHandle,
                                      g_focusDeathTick,
                                      now);
        TimingLog("[Director] focus-death target=%d killer=%d deathTick=%lu holdUntil=%lu state=%s now=%lu\n",
                  g_focusDeathTargetHandle,
                  g_focusDeathKillerHandle,
                  g_focusDeathTick,
                  g_focusDeathHoldUntil,
                  ShotKindName(g_committedShotKind),
                  now);
        return true;
    }

    if (!TickReached(now, g_focusDeathHoldUntil)) {
        return true;
    }

    RetargetAfterFocusDeath(now);
    return true;
}

// ============================================================================
// Event Processing
// ============================================================================

void CameraDirector_OnKill(int killerHandle, int victimHandle, int weaponId, int leadMs) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();
    if (leadMs < 0) leadMs = 0;

    DirectorEvent event = {};
    event.type = DirectorEventType::Kill;
    event.sequence = ++g_compatEventSequence;
    event.rawTick = now;
    event.visibleTick = now + (DWORD)leadMs;
    event.playbackDelayMs = leadMs;
    event.availableLeadMs = leadMs;
    event.killerHandle = killerHandle;
    event.victimHandle = victimHandle;
    event.weaponId = weaponId;
    QueueDirectorEvent(event, "compat-kill");
}

void CameraDirector_OnFlagChanged(int usCarrier, int vcCarrier, int leadMs) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();
    if (leadMs < 0) leadMs = 0;

    DirectorEvent event = {};
    event.type = DirectorEventType::Flag;
    event.sequence = ++g_compatEventSequence;
    event.rawTick = now;
    event.visibleTick = now + (DWORD)leadMs;
    event.playbackDelayMs = leadMs;
    event.availableLeadMs = leadMs;
    event.usCarrier = usCarrier;
    event.vcCarrier = vcCarrier;
    QueueDirectorEvent(event, "compat-flag");
    return;

#if 0
    g_flagCarrierUS = usCarrier;
    g_flagCarrierVC = vcCarrier;
    g_lastEventTick = now;
    UpdatePlayerActivity(g_lastEventTick);
    BumpPlayerActivity(usCarrier, 0.6f);
    BumpPlayerActivity(vcCarrier, 0.6f);
    UpdateFlagCarrierMotion(g_lastEventTick);

    bool anyFlag = (usCarrier != 0 || vcCarrier != 0);
    bool dualFlag = (usCarrier != 0 && vcCarrier != 0);

    if (anyFlag) {
        g_flagLostGraceActive = false;
        /*
            int target = 0;
            if (usCarrier != 0 && vcCarrier == 0) target = usCarrier;
            else if (vcCarrier != 0 && usCarrier == 0) target = vcCarrier;
            else target = usCarrier; // both — pick one
            // Legacy drone flag handoff removed.
            CD_Log("[CameraDirector] Drone: flag event -> tracking carrier=%d\n", target);
            return;
        */

        // Pick which carrier to watch
        int target = 0;
        if (usCarrier != 0 && vcCarrier == 0) target = usCarrier;
        else if (vcCarrier != 0 && usCarrier == 0) target = vcCarrier;
        else {
            // Both flags taken: keep the current carrier until timed alternation.
            if (IsUsableActiveFlagCarrier(g_currentTargetHandle)) {
                target = g_currentTargetHandle;
            } else if (IsUsableActiveFlagCarrier(usCarrier)) {
                target = usCarrier;
            } else if (IsUsableActiveFlagCarrier(vcCarrier)) {
                target = vcCarrier;
            } else {
                target = usCarrier;
            }

            if (!wasDualFlag) {
                g_flagAlternateTimer = now;
                g_flagAlternateDelay = RandomRange(FLAG_ALTERNATE_MIN_SEC, FLAG_ALTERNATE_MAX_SEC);
            }
        }

        if (target != 0
            && (g_state != CameraState::FlagWatch || g_currentTargetHandle != target)) {
            if (RequestFlagWatchShot(target,
                                     dualFlag ? "flag carrier dual" : "flag carrier",
                                     now)) {
                FeedNextFlagCarry(target, FlagNameForCarrier(target, usCarrier, vcCarrier), leadMs);
            }
        }
    } else {
        // No flags — start grace period
        if (g_state == CameraState::FlagWatch && !g_flagLostGraceActive) {
            g_flagLostGraceActive = true;
            g_flagLostTimestamp = now;
            CD_Log("[CameraDirector] Flag lost - grace period started\n");

            const char* lostFlag = "flag";
            if (previousUSCarrier != 0 && previousVCCarrier != 0) {
                lostFlag = "US and VC flags";
            } else if (previousUSCarrier != 0) {
                lostFlag = "US flag";
            } else if (previousVCCarrier != 0) {
                lostFlag = "VC flag";
            }
            FeedNextFlagLost(lostFlag);
        }
    }
#endif
}

// ============================================================================
// Per-Frame Update
// ============================================================================

void CameraDirector_Update() {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();
    DrainSnifferEvents();
    PlayerLifeTracker_UpdateFromFrame(now);
    if (g_flagScoreHoldTarget != 0 && !IsFlagScoreHoldActive(now)) {
        ClearFlagScoreHold(now, "expired");
    }
    PromoteQueuedEvents(now);

    UpdatePlayerActivity(now);
    UpdateFlagCarrierMotion(now);
    if (HandleCurrentFocusDeath(now)) {
        CommentaryEngine_Update(BuildCommentaryContext(now));
        g_lastUpdateTick = now;
        return;
    }
    CommentaryEngine_Update(BuildCommentaryContext(now));

    if (EnsureFlagScoreHoldCamera(now)) {
        g_lastUpdateTick = now;
        return;
    }

    if (g_flagCampingGlimpseHandle != 0
        && TickReached(now, g_flagCampingGlimpseUntil)) {
        int glimpseHandle = g_flagCampingGlimpseHandle;
        ClearFlagCampingGlimpse();
        if (g_state == CameraState::FlagWatch
            && g_currentTargetHandle == glimpseHandle
            && !IsShotHoldActive(now)
            && IsCampingFlagCarrier(glimpseHandle)) {
            ClearCommittedShot(now, "flag camper glimpse complete");
            StartNextScheduledShot(now, "flag camper glimpse complete");
        }
    }

    if (!IsDebugCameraModeActive()) {
        int movingFlagTarget = PickMovingFlagCarrierTarget();
        if (movingFlagTarget != 0
            && (g_state != CameraState::FlagWatch
                || g_currentTargetHandle != movingFlagTarget)) {
            ClearFlagCampingGlimpse();
            RequestFlagWatchShot(movingFlagTarget, "flag carrier moving", now);
        } else if (movingFlagTarget == 0) {
            TryStartFlagCampingGlimpse(now);
        }
    }

    if (g_flagKillLookKillTick != 0) {
        bool watchingVictim = g_state == CameraState::FlagWatch
            && g_currentTargetHandle == g_flagKillLookVictimHandle;
        if (!watchingVictim) {
            ClearFlagKillLook();
        }
    }

    // --- Screen time accumulation ---
    if (g_lastUpdateTick != 0) {
        float dt = (now - g_lastUpdateTick) / 1000.0f;
        if (dt > 0.0f && dt < 1.0f) { // sanity: skip huge gaps
            if (g_state == CameraState::Drone) {
                g_droneCamTime += dt;
            } else if (g_state == CameraState::FollowPlayer || g_state == CameraState::Idle) {
                int wcType = WorldCameraTracker_GetCurrentCamType();
                if (wcType == 0 || wcType == 1) {
                    g_worldCamTime += dt;
                } else {
                    g_playerCamTime += dt;
                }
            }
            // KillCam and FlagWatch don't count toward discretionary time
        }
    }
    g_lastUpdateTick = now;

    // --- Periodic screen time log (every 30s) ---
    if (now - g_lastScreenTimeLog >= 30000) {
        float total = g_playerCamTime + g_worldCamTime + g_droneCamTime;
        if (total > 1.0f) {
            CD_Log("[CameraDirector] Screen time: P:%d%% W:%d%% D:%d%%\n",
                   (int)(g_playerCamTime / total * 100),
                   (int)(g_worldCamTime / total * 100),
                   (int)(g_droneCamTime / total * 100));
        }
        g_lastScreenTimeLog = now;
    }

    // --- State-specific logic ---
    switch (g_state) {
        case CameraState::KillCam:
            UpdateKillCamPhase();
            break;

        case CameraState::FlagWatch: {
            // Handle flag lost grace period
            if (g_flagLostGraceActive) {
                float elapsed = (now - g_flagLostTimestamp) / 1000.0f;
                if (elapsed > g_config.flagLostGracePeriod && !IsShotHoldActive(now)) {
                    g_flagLostGraceActive = false;
                    ClearCommittedShot(now, "flag grace expired");
                    StartNextScheduledShot(now, "flag grace expired");
                    CD_Log("[CameraDirector] Flag grace expired, returning to kills\n");
                    break;
                }
            }

            bool dualFlagActive = (g_flagCarrierUS != 0 && g_flagCarrierVC != 0);
            if (dualFlagActive && !IsUsableActiveFlagCarrier(g_currentTargetHandle)) {
                if (CurrentCommittedShotUsable(now)) {
                    break;
                }
                int replacement = OtherFlagCarrierHandle(g_currentTargetHandle);
                if (IsUsableActiveFlagCarrier(replacement)
                    && RequestFlagWatchShot(replacement, "dual flag current invalid", now)) {
                    ClearFlagCampingGlimpse();
                    break;
                }
            }

            if (IsCampingFlagCarrier(g_currentTargetHandle)) {
                bool inGlimpse = g_flagCampingGlimpseHandle == g_currentTargetHandle
                    && g_flagCampingGlimpseUntil != 0
                    && !TickReached(now, g_flagCampingGlimpseUntil);
                if (inGlimpse) {
                    break;
                }
                // If we are already watching an active flag carrier, do not cut
                // away merely because they paused or slowed near the base.
                // The camera should stay with the run until the flag is dropped,
                // captured, or replaced by another valid flag carrier.
                break;
            }

            // Handle dual-flag alternation.
            if (dualFlagActive && IsUsableActiveFlagCarrier(g_currentTargetHandle)) {
                float elapsed = (now - g_flagAlternateTimer) / 1000.0f;
                if (elapsed > g_flagAlternateDelay) {
                    int nextTarget = OtherFlagCarrierHandle(g_currentTargetHandle);
                    DWORD previousFlagSwitch = g_lastFlagWatchTargetSwitch;
                    if (IsUsableActiveFlagCarrier(nextTarget)
                        && RequestFlagWatchShot(nextTarget, "dual flag alternate", now)
                        && g_currentTargetHandle == nextTarget
                        && g_lastFlagWatchTargetSwitch != previousFlagSwitch) {
                        g_flagAlternateTimer = now;
                        g_flagAlternateDelay = RandomRange(FLAG_ALTERNATE_MIN_SEC, FLAG_ALTERNATE_MAX_SEC);
                    }
                }
            }
            break;
        }

        case CameraState::Drone: {
            if (!CD_IsAliveUsablePlayerHandle(g_currentTargetHandle)) {
                if (CurrentCommittedShotUsable(now)) {
                    break;
                }
                ClearCommittedShot(now, "drone target lost");
                StartNextScheduledShot(now, "drone target lost");
                break;
            }

            if (!IsShotHoldActive(now)) {
                StartNextScheduledShot(now, "drone hold expired");
            }
            break;
        }

        case CameraState::FollowPlayer:
        case CameraState::Idle: {
            bool noFlagPriority = IsDebugCameraModeActive()
                || (g_flagCarrierUS == 0 && g_flagCarrierVC == 0);
            bool currentTargetUsable = CD_IsAliveUsablePlayerHandle(g_currentTargetHandle);

            if (noFlagPriority && !currentTargetUsable) {
                if (CurrentCommittedShotUsable(now)) {
                    break;
                }
                int picked = PickRandomActivePlayer(0);
                if (picked != 0) {
                    RequestFollowPlayerShot(picked,
                                            ActiveDebugCameraMode() == DebugCameraMode::World ? CAM_WORLD : CAM_PLAYER,
                                            true,
                                            "target lost",
                                            now);
                } else {
                    ClearCommittedShot(now, "target lost and no active players");
                }
                break;
            }

            if (noFlagPriority && !IsShotHoldActive(now)) {
                if (StartNextScheduledShot(now, "shot hold expired")) {
                    break;
                }
            }

            /*
                // If already on the needed type, keep going — hold resets on actual switch
            */
            break;
        }
    }
}

void CameraDirector_OnSpectatorFrame(int* spectObj, float deltaTime) {
    CameraDirector_Update();

    if (spectObj) {
        int debugWorldPreference = WorldPreferenceForDebugMode();
        if (debugWorldPreference != -2) {
            WorldCameraTracker_SetPreference(debugWorldPreference);
        }
        WorldCameraTracker_Update(spectObj, g_gameBase);
    }

    if (DroneCamera_IsActive()) {
        DroneCamera_Update(deltaTime);
    }
}

// ============================================================================
// State Accessors (for FirstPersonCamera hook)
// ============================================================================

CameraState CameraDirector_GetState() { return g_state; }
KillCamPhase CameraDirector_GetKillCamPhase() { return g_killCamPhase; }
KillCamStyle CameraDirector_GetKillCamStyle() { return g_kcStyle; }
int CameraDirector_GetTargetHandle() { return g_currentTargetHandle; }
int CameraDirector_GetKillCamKillerHandle() { return g_kcKillerHandle; }
int CameraDirector_GetKillCamVictimHandle() { return g_kcVictimHandle; }
bool CameraDirector_GetKillCamVictimAimPoint(float out[3]) {
    if (!out || !g_kcVictimAimPointValid) return false;
    out[0] = g_kcVictimAimPoint[0];
    out[1] = g_kcVictimAimPoint[1];
    out[2] = g_kcVictimAimPoint[2];
    return true;
}
float CameraDirector_GetKillCamElapsed() { return GetKillCamElapsedInternal(); }
const CameraConfig& CameraDirector_GetConfig() { return g_config; }
bool CameraDirector_ShouldUseFpv() { return g_currentShotUseFpv; }
const char* CameraDirector_GetActiveCameraModeName() {
    switch (g_state) {
        case CameraState::Idle:
            return "Idle";
        case CameraState::FollowPlayer:
            return ActivePlayerCameraModeName();
        case CameraState::KillCam:
            return g_kcStyle == KillCamStyle::BulletTravel
                ? "KillCam Bullet"
                : "KillCam Victim 3PV";
        case CameraState::FlagWatch:
            return ActiveFlagCameraModeName();
        case CameraState::Drone:
            return "Drone";
        default:
            return "Unknown";
    }
}

extern "C" __declspec(dllexport) int __cdecl VCStreamer_CopyActiveCameraModeName(char* out, int outSize) {
    if (out && outSize > 0) {
        out[0] = '\0';
    }

    if (!out || outSize <= 0 || !ShouldExposeActiveCameraModeName()) {
        return 0;
    }

    const char* name = CameraDirector_GetActiveCameraModeName();
    if (!name || name[0] == '\0') {
        return 0;
    }

    strncpy_s(out, static_cast<size_t>(outSize), name, _TRUNCATE);
    return 1;
}

bool CameraDirector_GetFlagCarrierKillLook(int* killerHandle,
                                           int* victimHandle,
                                           float* elapsed,
                                           float* duration) {
    DWORD now = GetTickCount();
    if (g_flagKillLookStartTick == 0
        || g_flagKillLookKillTick == 0) {
        return false;
    }

    bool watchingVictim = g_state == CameraState::FlagWatch
        && g_currentTargetHandle == g_flagKillLookVictimHandle;
    if (!watchingVictim || !TickReached(now, g_flagKillLookStartTick)) {
        return false;
    }

    float dur = (g_flagKillLookKillTick - g_flagKillLookStartTick) / 1000.0f;
    if (dur < 0.1f) dur = 0.1f;
    float el = (now - g_flagKillLookStartTick) / 1000.0f;

    if (killerHandle) *killerHandle = g_flagKillLookKillerHandle;
    if (victimHandle) *victimHandle = g_flagKillLookVictimHandle;
    if (elapsed) *elapsed = el;
    if (duration) *duration = dur;
    return true;
}

bool CameraDirector_GetFlagCarrierKillLookAimPoint(float out[3]) {
    if (!out || !g_flagKillLookKillerAimPointValid) return false;

    DWORD now = GetTickCount();
    bool watchingVictim = g_state == CameraState::FlagWatch
        && g_currentTargetHandle == g_flagKillLookVictimHandle;
    if (g_flagKillLookStartTick == 0
        || !watchingVictim
        || !TickReached(now, g_flagKillLookStartTick)) {
        return false;
    }

    out[0] = g_flagKillLookKillerAimPoint[0];
    out[1] = g_flagKillLookKillerAimPoint[1];
    out[2] = g_flagKillLookKillerAimPoint[2];
    return true;
}

// ============================================================================
// Lifecycle
// ============================================================================

void InitCameraDirector(uintptr_t gameBase) {
    g_gameBase = gameBase;
    InitPlayerLifeTracker(gameBase);
    CommentaryEngine_Init(gameBase);
    srand((unsigned int)time(nullptr));

    DroneCamera_Init(gameBase);
    LoadCameraDirectorConfig(g_config);
    WorldCameraTracker_SetTuning(g_config.worldCamMaxDistance,
                                 g_config.worldCamSwitchCooldown,
                                 g_config.worldCamMaxHold,
                                 g_config.worldCamLOSPenalty,
                                 g_config.worldCamStickiness,
                                 g_config.worldCamScoreThreshold);
    g_lastEventTick = GetTickCount();
    g_lastUpdateTick = GetTickCount();
    g_currentHoldStart = GetTickCount();
    g_shotHoldUntil = 0;
    g_committedShotKind = ShotKind::None;
    g_committedPreference = CAM_WORLD;
    g_eventQueue.clear();
    g_currentInvalidSince = 0;
    g_compatEventSequence = 0;
    g_currentShotUseFpv = false;
    g_focusDeathTargetHandle = 0;
    g_focusDeathKillerHandle = 0;
    g_focusDeathTick = 0;
    g_focusDeathHoldUntil = 0;
    g_kcStyle = KillCamStyle::BulletTravel;
    g_flagScoreHoldTarget = 0;
    g_flagScoreHoldUntil = 0;
    g_flagAlternateDelay = RandomRange(FLAG_ALTERNATE_MIN_SEC, FLAG_ALTERNATE_MAX_SEC);
    g_lastFlagWatchTargetSwitch = 0;
    g_lastScreenTimeLog = GetTickCount();
    g_playerCamTime = 0.0f;
    g_worldCamTime = 0.0f;
    g_droneCamTime = 0.0f;

    CD_Log("[CameraDirector] Initialized\n");
    CD_Log("[CameraDirector] KillCam split: victim-lock 3PV < %.2fm (%d%%), vertical detach %d%%, bullet >= %.2fm (%d%%), look-lock %.2fs before kill %.2fs after kill\n",
           g_config.killCamLongRangeDistance,
           (int)(g_config.detachedKillCamChance * 100),
           (int)(g_config.detachedKillCamVantageChance * 100),
           g_config.killCamLongRangeDistance,
           (int)(g_config.bulletKillCamChance * 100),
           g_config.killLookLockAdvance,
           g_config.killLookLockPostKillDuration);
    CD_Log("[CameraDirector] Budget: P=%d%% W=%d%% D=%d%% droneMinClearance=%.2f\n",
           (int)(g_config.camSharePlayer * 100),
           (int)(g_config.camShareWorld * 100),
           (int)(g_config.camShareDrone * 100),
           g_config.droneMinAreaClearance);
    if (IsDebugCameraModeActive()) {
        CD_Log("[CameraDirector] Debug camera mode: %s\n",
               DebugCameraModeName(ActiveDebugCameraMode()));
    }
    TimingLog("[Director] initialized preRollMs=%d\n",
              (int)(g_config.directorPreRollSeconds * 1000.0f));
}
