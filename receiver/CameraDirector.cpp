#define _CRT_SECURE_NO_WARNINGS
#include "CameraDirector.h"
#include "DelayManager.h"
#include "DroneCamera.h"
#include "PathGrid.h"
#include "SpectatorController.h"
#include "WorldCameraTracker.h"
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <mutex>
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

static constexpr float NORMAL_SHOT_MIN_HOLD_SEC = 20.0f;
static constexpr float NORMAL_SHOT_MAX_HOLD_SEC = 25.0f;
static constexpr float FLAG_ALTERNATE_MIN_SEC = 8.0f;
static constexpr float FLAG_ALTERNATE_MAX_SEC = 12.0f;

// KillCam state
static KillCamPhase g_killCamPhase = KillCamPhase::Wait;
static int g_kcKillerHandle = 0;
static int g_kcVictimHandle = 0;
static DWORD g_kcStartTick = 0;

// Flag state
static int g_flagCarrierUS = 0;
static int g_flagCarrierVC = 0;
static DWORD g_flagAlternateTimer = 0;
static float g_flagAlternateDelay = 10.0f;

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
    CamBudgetType preference;
    float holdSeconds;
    bool force;
    const char* reason;
};

static ShotKind g_committedShotKind = ShotKind::None;
static CamBudgetType g_committedPreference = CAM_WORLD;

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

static int PickRandomActivePlayer(int avoidHandle) {
    if (!g_gameBase) return 0;

    std::vector<int> handles;
    handles.reserve(64);

    void** playerTable = (void**)(g_gameBase + 0x7AE9C8);
    for (int i = 0; i < 64; ++i) {
        void* entry = playerTable[i];
        if (!CD_IsUsablePlayerEntry(entry)) continue;

        int handle = *(int*)entry;
        if (handle != avoidHandle) {
            handles.push_back(handle);
        }
    }

    if (handles.empty() && avoidHandle != 0 && CD_IsUsablePlayerHandle(avoidHandle)) {
        return avoidHandle;
    }
    if (handles.empty()) return 0;

    return handles[(size_t)(rand() % (int)handles.size())];
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
    int chance = DelayManager::GetFpvChance();
    if (chance <= 0) return false;
    if (chance >= 100) return true;
    return (rand() % 100) < chance;
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

static float GetKillCamTotalDuration() {
    return g_config.killCamWaitDuration
         + g_config.killCamTransitionDuration
         + g_config.killCamAttachedDuration;
}

static bool IsKillCamInterruptible() {
    float elapsed = GetKillCamElapsedInternal();
    float waitEnd = g_config.killCamWaitDuration;
    float transEnd = waitEnd + g_config.killCamTransitionDuration;

    if (elapsed < waitEnd) return true;
    if (elapsed < transEnd) return false;
    if (elapsed > transEnd + 1.0f) return true;
    return false;
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
    g_kcKillerHandle = 0;
    g_kcVictimHandle = 0;
    WorldCameraTracker_ClearTarget();

    std::cout << "[CameraDirector] Shot cleared: " << (reason ? reason : "none") << "\n";
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
            g_kcKillerHandle = 0;
            g_kcVictimHandle = 0;

            SetSpectatorToPlayerId(request.targetHandle);
            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(request.preference == CAM_PLAYER ? 2 : 0);

            std::cout << "[CameraDirector] Commit follow target=" << request.targetHandle
                      << " pref=" << BudgetName(request.preference)
                      << " hold=" << (int)((g_shotHoldUntil - now) / 1000)
                      << "s fpv=" << (g_currentShotUseFpv ? "yes" : "no")
                      << " reason=" << (request.reason ? request.reason : "none") << "\n";
            return true;
        }

        case ShotKind::KillCam: {
            if (!CD_IsUsablePlayerHandle(request.targetHandle)) return false;

            g_state = CameraState::KillCam;
            g_committedShotKind = ShotKind::KillCam;
            g_committedPreference = CAM_WORLD;
            g_killCamPhase = KillCamPhase::Wait;
            g_kcKillerHandle = request.targetHandle;
            g_kcVictimHandle = request.victimHandle;
            g_kcStartTick = now;
            g_currentTargetHandle = request.targetHandle;
            g_currentHoldStart = now;
            g_shotHoldUntil = HoldUntil(now, request.holdSeconds);
            g_currentShotUseFpv = false;

            SetSpectatorToPlayerId(request.targetHandle);
            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(0);

            std::cout << "[CameraDirector] Commit killcam killer=" << request.targetHandle
                      << " victim=" << request.victimHandle
                      << " reason=" << (request.reason ? request.reason : "none") << "\n";
            return true;
        }

        case ShotKind::FlagWatch: {
            if (request.targetHandle == 0) return false;

            g_state = CameraState::FlagWatch;
            g_committedShotKind = ShotKind::FlagWatch;
            g_committedPreference = CAM_WORLD;
            g_currentTargetHandle = request.targetHandle;
            g_currentHoldStart = now;
            g_shotHoldUntil = 0;
            g_currentShotUseFpv = RollFpvForShot();
            g_kcKillerHandle = 0;
            g_kcVictimHandle = 0;

            SetSpectatorToPlayerId(request.targetHandle);
            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(0);

            std::cout << "[CameraDirector] Commit flag target=" << request.targetHandle
                      << " fpv=" << (g_currentShotUseFpv ? "yes" : "no")
                      << " reason=" << (request.reason ? request.reason : "none") << "\n";
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
            g_kcKillerHandle = 0;
            g_kcVictimHandle = 0;

            WorldCameraTracker_SetTarget(request.targetHandle);
            WorldCameraTracker_SetPreference(-1);

            std::cout << "[CameraDirector] Commit drone target=" << request.targetHandle
                      << " hold=" << (int)((g_shotHoldUntil - now) / 1000)
                      << "s reason=" << (request.reason ? request.reason : "none") << "\n";
            return true;
        }

        default:
            return false;
    }
}

static bool RequestShot(const ShotRequest& request, DWORD now) {
    if (!request.force) {
        if (g_state == CameraState::FlagWatch) {
            std::cout << "[CameraDirector] Shot request ignored during flag watch: "
                      << ShotKindName(request.kind) << "\n";
            return false;
        }

        if (IsCommittedShotLocked(now)) {
            std::cout << "[CameraDirector] Shot request ignored: locked "
                      << ShotKindName(g_committedShotKind)
                      << " remaining=" << (int)((g_shotHoldUntil - now) / 1000)
                      << "s request=" << ShotKindName(request.kind) << "\n";
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
        preference,
        NormalFollowHoldSeconds(preference),
        force,
        reason
    };
    return RequestShot(request, now);
}

static bool RequestKillCamShot(int killerHandle,
                               int victimHandle,
                               bool force,
                               const char* reason,
                               DWORD now) {
    ShotRequest request = {
        ShotKind::KillCam,
        killerHandle,
        victimHandle,
        CAM_WORLD,
        GetKillCamTotalDuration(),
        force,
        reason
    };
    return RequestShot(request, now);
}

static bool RequestFlagWatchShot(int carrierHandle,
                                 const char* reason,
                                 DWORD now) {
    ShotRequest request = {
        ShotKind::FlagWatch,
        carrierHandle,
        0,
        CAM_WORLD,
        0.0f,
        true,
        reason
    };
    return RequestShot(request, now);
}

static bool RequestDroneShot(int targetHandle,
                             bool force,
                             const char* reason,
                             DWORD now) {
    ShotRequest request = {
        ShotKind::Drone,
        targetHandle,
        0,
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

    std::cout << "[CameraDirector] KillCam ended\n";

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
    int currentTarget = CD_IsUsablePlayerHandle(g_currentTargetHandle) ? g_currentTargetHandle : 0;

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
    float waitEnd = g_config.killCamWaitDuration;
    float transEnd = waitEnd + g_config.killCamTransitionDuration;
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

void CameraDirector_OnKill(int killerHandle, int victimHandle) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();
    g_lastEventTick = now;

    if (g_state == CameraState::FlagWatch) {
        std::cout << "[CameraDirector] Kill ignored: flag shot committed\n";
        return;
    }

    if (IsCommittedShotLocked(now)) {
        std::cout << "[CameraDirector] Kill ignored: committed shot hold active\n";
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

    // Measure kill distance
    float distance = GetKillDistance(killerHandle, victimHandle);

    // Compute cinematic chance based on distance
    float chance = ComputeCinematicChance(distance);
    bool cinematic = RollCinematic(chance);

    std::cout << "[CameraDirector] Kill: killer=" << killerHandle
              << " victim=" << victimHandle
              << " distance=" << distance << "m"
              << " cinematicChance=" << (int)(chance * 100) << "%"
              << " result=" << (cinematic ? "CINEMATIC" : "NORMAL") << "\n";

    if (cinematic) {
        if (RequestKillCamShot(killerHandle, victimHandle, false, "cinematic kill", now)) {
            g_lastKillSwitch = now;
        }
    } else if (CD_IsUsablePlayerHandle(killerHandle)) {
        if (RequestFollowPlayerShot(killerHandle, CAM_WORLD, false, "normal kill", now)) {
            g_lastKillSwitch = now;
        }
    }
}

void CameraDirector_OnFlagChanged(int usCarrier, int vcCarrier) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    g_flagCarrierUS = usCarrier;
    g_flagCarrierVC = vcCarrier;
    g_lastEventTick = GetTickCount();

    bool anyFlag = (usCarrier != 0 || vcCarrier != 0);

    if (anyFlag) {
        g_flagLostGraceActive = false;
        /*
            int target = 0;
            if (usCarrier != 0 && vcCarrier == 0) target = usCarrier;
            else if (vcCarrier != 0 && usCarrier == 0) target = vcCarrier;
            else target = usCarrier; // both — pick one
            // Legacy drone flag handoff removed.
            std::cout << "[CameraDirector] Drone: flag event -> tracking carrier=" << target << "\n";
            return;
        */

        // Pick which carrier to watch
        int target = 0;
        if (usCarrier != 0 && vcCarrier == 0) target = usCarrier;
        else if (vcCarrier != 0 && usCarrier == 0) target = vcCarrier;
        else {
            // Both flags taken — watch the one we're not currently on
            target = (g_currentTargetHandle == usCarrier) ? vcCarrier : usCarrier;
            g_flagAlternateTimer = GetTickCount();
            g_flagAlternateDelay = RandomRange(FLAG_ALTERNATE_MIN_SEC, FLAG_ALTERNATE_MAX_SEC);
        }

        RequestFlagWatchShot(target, "flag carrier", GetTickCount());
    } else {
        // No flags — start grace period
        if (g_state == CameraState::FlagWatch && !g_flagLostGraceActive) {
            g_flagLostGraceActive = true;
            g_flagLostTimestamp = GetTickCount();
            std::cout << "[CameraDirector] Flag lost - grace period started\n";
        }
    }
}

// ============================================================================
// Per-Frame Update
// ============================================================================

void CameraDirector_Update() {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();

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
            std::cout << "[CameraDirector] Screen time: P:"
                      << (int)(g_playerCamTime / total * 100) << "% W:"
                      << (int)(g_worldCamTime / total * 100) << "% D:"
                      << (int)(g_droneCamTime / total * 100) << "%\n";
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
                    std::cout << "[CameraDirector] Flag grace expired, returning to kills\n";
                    break;
                }
            }

            // Handle dual-flag alternation.
            if (g_flagCarrierUS != 0 && g_flagCarrierVC != 0) {
                float elapsed = (now - g_flagAlternateTimer) / 1000.0f;
                if (elapsed > g_flagAlternateDelay) {
                    int nextTarget = (g_currentTargetHandle == g_flagCarrierUS)
                                   ? g_flagCarrierVC : g_flagCarrierUS;
                    RequestFlagWatchShot(nextTarget, "dual flag alternate", now);
                    g_flagAlternateTimer = now;
                    g_flagAlternateDelay = RandomRange(FLAG_ALTERNATE_MIN_SEC, FLAG_ALTERNATE_MAX_SEC);
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
int CameraDirector_GetTargetHandle() { return g_currentTargetHandle; }
int CameraDirector_GetKillCamKillerHandle() { return g_kcKillerHandle; }
int CameraDirector_GetKillCamVictimHandle() { return g_kcVictimHandle; }
float CameraDirector_GetKillCamElapsed() { return GetKillCamElapsedInternal(); }
const CameraConfig& CameraDirector_GetConfig() { return g_config; }
bool CameraDirector_ShouldUseFpv() { return g_currentShotUseFpv; }

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
                                 g_config.worldCamStickiness);
    g_lastEventTick = GetTickCount();
    g_lastUpdateTick = GetTickCount();
    g_currentHoldStart = GetTickCount();
    g_shotHoldUntil = 0;
    g_committedShotKind = ShotKind::None;
    g_committedPreference = CAM_WORLD;
    g_currentShotUseFpv = false;
    g_flagAlternateDelay = RandomRange(FLAG_ALTERNATE_MIN_SEC, FLAG_ALTERNATE_MAX_SEC);
    g_lastScreenTimeLog = GetTickCount();
    g_playerCamTime = 0.0f;
    g_worldCamTime = 0.0f;
    g_droneCamTime = 0.0f;

    std::cout << "[CameraDirector] Initialized\n";
    std::cout << "[CameraDirector] Cinematic range: "
              << g_config.cinematicMinDistance << "m (" << (int)(g_config.cinematicMinChance * 100) << "%) -> "
              << g_config.cinematicMaxDistance << "m (" << (int)(g_config.cinematicMaxChance * 100) << "%)\n";
    std::cout << "[CameraDirector] Budget: P=" << (int)(g_config.camSharePlayer * 100)
              << "% W=" << (int)(g_config.camShareWorld * 100)
              << "% D=" << (int)(g_config.camShareDrone * 100)
              << "% droneMinClearance=" << g_config.droneMinAreaClearance << "\n";
}
