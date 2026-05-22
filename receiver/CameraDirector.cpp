#define _CRT_SECURE_NO_WARNINGS
#include "CameraDirector.h"
#include "CommentatorFeed.h"
#include "DelayManager.h"
#include "DiagnosticsLog.h"
#include "DroneCamera.h"
#include "PathGrid.h"
#include "RealtimeHook.h"
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

// KillCam state
static KillCamPhase g_killCamPhase = KillCamPhase::Wait;
static KillCamStyle g_kcStyle = KillCamStyle::BulletTravel;
static int g_kcKillerHandle = 0;
static int g_kcVictimHandle = 0;
static DWORD g_kcStartTick = 0;

// Flag state
static int g_flagCarrierUS = 0;
static int g_flagCarrierVC = 0;
static DWORD g_flagAlternateTimer = 0;
static float g_flagAlternateDelay = 10.0f;
static DWORD g_lastFlagWatchTargetSwitch = 0;
static int g_flagKillLookKillerHandle = 0;
static int g_flagKillLookVictimHandle = 0;
static DWORD g_flagKillLookStartTick = 0;
static DWORD g_flagKillLookKillTick = 0;
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
};

static ShotKind g_committedShotKind = ShotKind::None;
static CamBudgetType g_committedPreference = CAM_WORLD;

static void CD_Log(const char* fmt, ...) {
    FILE* file = fopen("receiver_debug.log", "a");
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
    if (handle == 0 || !CD_IsUsablePlayerHandle(handle)) {
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
        && CD_IsUsablePlayerHandle(handle)
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
        && CD_IsUsablePlayerHandle(g_flagCarrierUS)
        && !IsCampingFlagCarrier(g_flagCarrierUS);
    bool vcMoving = g_flagCarrierVC != 0
        && CD_IsUsablePlayerHandle(g_flagCarrierVC)
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

    if (candidates.empty() && avoidHandle != 0 && CD_IsUsablePlayerHandle(avoidHandle)) {
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
    if (!CD_IsUsablePlayerHandle(g_currentTargetHandle)) return false;

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

static bool GetBestKillPosition(int handle, float* out, bool* usedPredicted) {
    if (usedPredicted) *usedPredicted = false;

    if (RealtimeHook_GetPredictedPlayerPosition(handle, out, 7000)) {
        if (usedPredicted) *usedPredicted = true;
        return true;
    }

    return GetCurrentPlayerPosition(handle, out);
}

static float GetKillDistance(int killerHandle, int victimHandle, bool* usedPredicted) {
    if (usedPredicted) *usedPredicted = false;

    bool killerPredicted = false;
    bool victimPredicted = false;
    float kp[3], vp[3];
    if (!GetBestKillPosition(killerHandle, kp, &killerPredicted)) return 0.0f;
    if (!GetBestKillPosition(victimHandle, vp, &victimPredicted)) return 0.0f;

    if (usedPredicted) *usedPredicted = killerPredicted || victimPredicted;

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

static bool IsKillCamInterruptible() {
    float elapsed = GetKillCamElapsedInternal();
    float waitEnd = GetKillCamWaitDuration(g_kcStyle);
    float transEnd = waitEnd + GetKillCamTransitionDuration(g_kcStyle);

    if (elapsed < waitEnd) return true;
    if (elapsed < transEnd) return false;
    if (elapsed > transEnd + 1.0f) return true;
    return false;
}

static bool TickReached(DWORD now, DWORD tick) {
    return (LONG)(now - tick) >= 0;
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
}

static bool MaybeStartVictimKillLook(int killerHandle, int victimHandle, DWORD now) {
    bool victimIsFlagCarrier = (victimHandle != 0)
        && (victimHandle == g_flagCarrierUS || victimHandle == g_flagCarrierVC);
    bool watchingVictim = (g_state == CameraState::FlagWatch
        || g_state == CameraState::FollowPlayer)
        && g_currentTargetHandle == victimHandle;

    if (!watchingVictim || killerHandle == 0) {
        return false;
    }

    float leadSeconds = GetKillLookLeadDuration();
    float advanceSeconds = GetKillLookAdvanceDuration();
    DWORD leadMs = (DWORD)(leadSeconds * 1000.0f + 0.5f);
    DWORD advanceMs = (DWORD)(advanceSeconds * 1000.0f + 0.5f);
    if (leadMs < advanceMs) leadMs = advanceMs;

    g_flagKillLookKillerHandle = killerHandle;
    g_flagKillLookVictimHandle = victimHandle;
    g_flagKillLookKillTick = now + leadMs;
    g_flagKillLookStartTick = g_flagKillLookKillTick - advanceMs;

    CD_Log("[CameraDirector] Victim death look: victim=%d killer=%d flagCarrier=%s startsIn=%ds, holds until camera changes\n",
           victimHandle,
           killerHandle,
           victimIsFlagCarrier ? "yes" : "no",
           (int)((leadMs - advanceMs) / 1000));
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

static const char* KillCamStyleName(KillCamStyle style) {
    return style == KillCamStyle::DetachedVantage ? "victim-lock-3pv" : "bullet";
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
        return "until-change";
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

static bool CurrentCommittedShotUsable() {
    switch (g_state) {
        case CameraState::FollowPlayer:
            return CD_IsUsablePlayerHandle(g_currentTargetHandle);
        case CameraState::FlagWatch:
            return g_currentTargetHandle != 0 && CD_IsUsablePlayerHandle(g_currentTargetHandle);
        case CameraState::Drone:
            return DroneCamera_IsActive() && CD_IsUsablePlayerHandle(g_currentTargetHandle);
        case CameraState::KillCam:
            return g_kcKillerHandle != 0;
        default:
            return false;
    }
}

static bool IsCommittedShotLocked(DWORD now) {
    if (g_state == CameraState::FlagWatch) return CurrentCommittedShotUsable();
    return IsShotHoldActive(now) && CurrentCommittedShotUsable();
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
    g_kcStyle = KillCamStyle::BulletTravel;
    g_kcKillerHandle = 0;
    g_kcVictimHandle = 0;
    ClearFlagKillLook();
    ClearFlagCampingGlimpse();
    WorldCameraTracker_ClearTarget();

    CD_Log("[CameraDirector] Shot cleared: %s\n", reason ? reason : "none");
}

static bool CommitShot(const ShotRequest& request, DWORD now) {
    if (request.kind != ShotKind::Drone && DroneCamera_IsActive()) {
        DroneCamera_Deactivate();
    }

    switch (request.kind) {
        case ShotKind::FollowPlayer: {
            if (!CD_IsUsablePlayerHandle(request.targetHandle)) return false;

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
            ClearFlagKillLook();
            ClearFlagCampingGlimpse();

            SetSpectatorToPlayerId(request.targetHandle);
            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(request.preference == CAM_PLAYER ? 2 : 0);

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
            if (!CD_IsUsablePlayerHandle(request.targetHandle)) return false;

            g_state = CameraState::KillCam;
            g_committedShotKind = ShotKind::KillCam;
            g_committedPreference = CAM_WORLD;
            g_killCamPhase = KillCamPhase::Wait;
            g_kcStyle = request.killCamStyle;
            g_kcKillerHandle = request.targetHandle;
            g_kcVictimHandle = request.victimHandle;
            g_kcStartTick = now;
            g_currentTargetHandle = request.targetHandle;
            g_currentHoldStart = now;
            g_shotHoldUntil = HoldUntil(now, request.holdSeconds);
            g_currentShotUseFpv = false;
            ClearFlagKillLook();
            ClearFlagCampingGlimpse();

            SetSpectatorToPlayerId(request.targetHandle);
            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(g_kcStyle == KillCamStyle::DetachedVantage ? 2 : 0);

            CD_Log("[CameraDirector] Commit killcam killer=%d victim=%d style=%s reason=%s\n",
                   request.targetHandle,
                   request.victimHandle,
                   KillCamStyleName(g_kcStyle),
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
            g_shotHoldUntil = 0;
            g_currentShotUseFpv = RollFpvForShot();
            g_kcStyle = KillCamStyle::BulletTravel;
            g_kcKillerHandle = 0;
            g_kcVictimHandle = 0;
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

            CD_Log("[CameraDirector] Commit flag target=%d fpv=%s reason=%s\n",
                   request.targetHandle,
                   g_currentShotUseFpv ? "yes" : "no",
                   request.reason ? request.reason : "none");
            FeedCurrentWatching(request, now);
            return true;
        }

        case ShotKind::Drone: {
            if (!CD_IsUsablePlayerHandle(request.targetHandle)) return false;
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
    if (!request.force) {
        if (g_state == CameraState::FlagWatch) {
            CD_Log("[CameraDirector] Shot request ignored during flag watch: %s\n",
                   ShotKindName(request.kind));
            return false;
        }

        if (IsCommittedShotLocked(now)) {
            CD_Log("[CameraDirector] Shot request ignored: locked %s remaining=%ds request=%s\n",
                   ShotKindName(g_committedShotKind),
                   (int)((g_shotHoldUntil - now) / 1000),
                   ShotKindName(request.kind));
            return false;
        }

        if (g_state == CameraState::KillCam && !IsKillCamInterruptible()) {
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
                               DWORD now) {
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
    return RequestShot(request, now);
}

static bool RequestFlagWatchShot(int carrierHandle,
                                 const char* reason,
                                 DWORD now) {
    if (carrierHandle != 0
        && g_state == CameraState::FlagWatch
        && g_currentTargetHandle == carrierHandle
        && CD_IsUsablePlayerHandle(carrierHandle)) {
        return true;
    }

    ShotRequest request = {
        ShotKind::FlagWatch,
        carrierHandle,
        0,
        KillCamStyle::BulletTravel,
        CAM_WORLD,
        0.0f,
        true,
        reason
    };
    return RequestShot(request, now);
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
        if (!CD_IsUsablePlayerHandle(motion->handle)) continue;
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
    g_lastEventTick = now;
    g_shotHoldUntil = 0;

    CD_Log("[CameraDirector] KillCam ended\n");

    if (CD_IsUsablePlayerHandle(nextTarget)) {
        RequestFollowPlayerShot(nextTarget, CAM_WORLD, true, "killcam complete", now);
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
    CamBudgetType needed = GetHighestDeficitType();
    int currentTarget = CD_IsUsablePlayerHandle(g_currentTargetHandle)
        && !IsCampingFlagCarrier(g_currentTargetHandle)
        ? g_currentTargetHandle
        : 0;

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
// Event Processing
// ============================================================================

void CameraDirector_OnKill(int killerHandle, int victimHandle, int weaponId, int leadMs) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();
    g_lastEventTick = now;
    UpdatePlayerActivity(now);
    BumpPlayerActivity(killerHandle, 0.8f);
    BumpPlayerActivity(victimHandle, 0.4f);

    bool usedPredictedPosition = false;
    float distance = GetKillDistance(killerHandle, victimHandle, &usedPredictedPosition);

    bool preservingVictimLook = MaybeStartVictimKillLook(killerHandle, victimHandle, now);
    if (preservingVictimLook) {
        if (g_state == CameraState::FollowPlayer
            && (g_shotHoldUntil == 0 || TickReached(g_flagKillLookKillTick, g_shotHoldUntil))) {
            g_shotHoldUntil = g_flagKillLookKillTick;
        }

        CD_Log("[CameraDirector] Kill preserved: victim death look target=%d killer=%d\n",
               victimHandle, killerHandle);
        FeedNextKill(killerHandle, victimHandle, weaponId, leadMs, distance, "victim-look");
        return;
    }

    if (g_state == CameraState::FlagWatch) {
        CD_Log("[CameraDirector] Kill ignored: flag shot committed\n");
        return;
    }

    // Respect cooldown
    if (now - g_lastKillSwitch < (DWORD)(g_config.killCooldown * 1000)) {
        return;
    }

    // Don't interrupt non-interruptible KillCam
    if (g_state == CameraState::KillCam && !IsKillCamInterruptible()) {
        return;
    }

    KillCamStyle style = (distance >= g_config.killCamLongRangeDistance)
        ? KillCamStyle::BulletTravel
        : KillCamStyle::DetachedVantage;

    float chance = (style == KillCamStyle::BulletTravel)
        ? g_config.bulletKillCamChance
        : g_config.detachedKillCamChance;
    bool cinematic = RollCinematic(chance);
    bool locked = IsCommittedShotLocked(now);
    float heldSeconds = (now - g_currentHoldStart) / 1000.0f;
    bool canInterruptHold = locked
        && cinematic
        && heldSeconds >= g_config.killInterruptMinHold;

    CD_Log("[CameraDirector] Kill: killer=%d victim=%d weapon=%d distance=%.2fm predicted=%s style=%s chance=%d%% locked=%s result=%s\n",
           killerHandle,
           victimHandle,
           weaponId,
           distance,
           usedPredictedPosition ? "yes" : "no",
           KillCamStyleName(style),
           (int)(chance * 100),
           locked ? "yes" : "no",
           cinematic ? "CINEMATIC" : "NORMAL");

    if (locked && !canInterruptHold) {
        CD_Log("[CameraDirector] Kill ignored: committed shot hold active\n");
        return;
    }

    if (cinematic) {
        bool force = canInterruptHold;
        if (RequestKillCamShot(killerHandle, victimHandle, style, force,
                               force ? "cinematic kill interrupt" : "cinematic kill",
                               now)) {
            g_lastKillSwitch = now;
            FeedNextKill(killerHandle, victimHandle, weaponId, leadMs, distance, "killcam");
        }
    } else if (CD_IsUsablePlayerHandle(killerHandle)) {
        if (RequestFollowPlayerShot(killerHandle, CAM_WORLD, false, "normal kill", now)) {
            g_lastKillSwitch = now;
            FeedNextKill(killerHandle, victimHandle, weaponId, leadMs, distance, "follow");
        }
    }
}

void CameraDirector_OnFlagChanged(int usCarrier, int vcCarrier, int leadMs) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    int previousUSCarrier = g_flagCarrierUS;
    int previousVCCarrier = g_flagCarrierVC;
    bool wasDualFlag = (g_flagCarrierUS != 0 && g_flagCarrierVC != 0);
    DWORD now = GetTickCount();

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
}

// ============================================================================
// Per-Frame Update
// ============================================================================

void CameraDirector_Update() {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();
    UpdatePlayerActivity(now);
    UpdateFlagCarrierMotion(now);

    if (g_flagCampingGlimpseHandle != 0
        && TickReached(now, g_flagCampingGlimpseUntil)) {
        int glimpseHandle = g_flagCampingGlimpseHandle;
        ClearFlagCampingGlimpse();
        if (g_state == CameraState::FlagWatch
            && g_currentTargetHandle == glimpseHandle
            && IsCampingFlagCarrier(glimpseHandle)) {
            ClearCommittedShot(now, "flag camper glimpse complete");
            StartNextScheduledShot(now, "flag camper glimpse complete");
        }
    }

    int movingFlagTarget = PickMovingFlagCarrierTarget();
    bool canInterruptForMovingFlag = !(g_state == CameraState::KillCam
        && !IsKillCamInterruptible());
    if (movingFlagTarget != 0
        && canInterruptForMovingFlag
        && (g_state != CameraState::FlagWatch
            || g_currentTargetHandle != movingFlagTarget)) {
        ClearFlagCampingGlimpse();
        RequestFlagWatchShot(movingFlagTarget, "flag carrier moving", now);
    } else if (movingFlagTarget == 0) {
        TryStartFlagCampingGlimpse(now);
    }

    if (g_flagKillLookKillTick != 0) {
        bool watchingVictim = (g_state == CameraState::FlagWatch
            || g_state == CameraState::FollowPlayer)
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
                if (elapsed > g_config.flagLostGracePeriod) {
                    g_flagLostGraceActive = false;
                    ClearCommittedShot(now, "flag grace expired");
                    StartNextScheduledShot(now, "flag grace expired");
                    CD_Log("[CameraDirector] Flag grace expired, returning to kills\n");
                    break;
                }
            }

            bool dualFlagActive = (g_flagCarrierUS != 0 && g_flagCarrierVC != 0);
            if (dualFlagActive && !IsUsableActiveFlagCarrier(g_currentTargetHandle)) {
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
                if (!inGlimpse) {
                    ClearFlagCampingGlimpse();
                    ClearCommittedShot(now, "flag carrier camping");
                    StartNextScheduledShot(now, "flag carrier camping");
                }
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
            if (!CD_IsUsablePlayerHandle(g_currentTargetHandle)) {
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
            bool noFlagPriority = (g_flagCarrierUS == 0 && g_flagCarrierVC == 0);
            bool currentTargetUsable = CD_IsUsablePlayerHandle(g_currentTargetHandle);

            if (noFlagPriority && !currentTargetUsable) {
                int picked = PickRandomActivePlayer(0);
                if (picked != 0) {
                    RequestFollowPlayerShot(picked, CAM_WORLD, true, "target lost", now);
                } else {
                    ClearCommittedShot(now, "target lost and no active players");
                }
                break;
            }

            if (noFlagPriority && g_state == CameraState::FollowPlayer && currentTargetUsable) {
                float heldSeconds = (now - g_currentHoldStart) / 1000.0f;
                float currentActivity = ActivityScoreForHandle(g_currentTargetHandle);
                if (heldSeconds >= 8.0f
                    && currentActivity < 0.04f
                    && HasActiveAlternative(g_currentTargetHandle)) {
                    g_shotHoldUntil = 0;
                    if (StartNextScheduledShot(now, "target idle")) {
                        break;
                    }
                }
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

// ============================================================================
// State Accessors (for FirstPersonCamera hook)
// ============================================================================

CameraState CameraDirector_GetState() { return g_state; }
KillCamPhase CameraDirector_GetKillCamPhase() { return g_killCamPhase; }
KillCamStyle CameraDirector_GetKillCamStyle() { return g_kcStyle; }
int CameraDirector_GetTargetHandle() { return g_currentTargetHandle; }
int CameraDirector_GetKillCamKillerHandle() { return g_kcKillerHandle; }
int CameraDirector_GetKillCamVictimHandle() { return g_kcVictimHandle; }
float CameraDirector_GetKillCamElapsed() { return GetKillCamElapsedInternal(); }
const CameraConfig& CameraDirector_GetConfig() { return g_config; }
bool CameraDirector_ShouldUseFpv() { return g_currentShotUseFpv; }
bool CameraDirector_GetFlagCarrierKillLook(int* killerHandle,
                                           int* victimHandle,
                                           float* elapsed,
                                           float* duration) {
    DWORD now = GetTickCount();
    if (g_flagKillLookStartTick == 0
        || g_flagKillLookKillTick == 0) {
        return false;
    }

    bool watchingVictim = (g_state == CameraState::FlagWatch
        || g_state == CameraState::FollowPlayer)
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

// ============================================================================
// Lifecycle
// ============================================================================

void InitCameraDirector(uintptr_t gameBase) {
    g_gameBase = gameBase;
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
    g_currentShotUseFpv = false;
    g_kcStyle = KillCamStyle::BulletTravel;
    g_flagAlternateDelay = RandomRange(FLAG_ALTERNATE_MIN_SEC, FLAG_ALTERNATE_MAX_SEC);
    g_lastFlagWatchTargetSwitch = 0;
    g_lastScreenTimeLog = GetTickCount();
    g_playerCamTime = 0.0f;
    g_worldCamTime = 0.0f;
    g_droneCamTime = 0.0f;

    CD_Log("[CameraDirector] Initialized\n");
    CD_Log("[CameraDirector] KillCam split: victim-lock 3PV < %.2fm (%d%%), bullet >= %.2fm (%d%%), look-lock %.2fs before kill\n",
           g_config.killCamLongRangeDistance,
           (int)(g_config.detachedKillCamChance * 100),
           g_config.killCamLongRangeDistance,
           (int)(g_config.bulletKillCamChance * 100),
           g_config.killLookLockAdvance);
    CD_Log("[CameraDirector] Budget: P=%d%% W=%d%% D=%d%% droneMinClearance=%.2f\n",
           (int)(g_config.camSharePlayer * 100),
           (int)(g_config.camShareWorld * 100),
           (int)(g_config.camShareDrone * 100),
           g_config.droneMinAreaClearance);
}
