#define _CRT_SECURE_NO_WARNINGS
#include "CameraDirector.h"
#include "DroneCamera.h"
#include "PathGrid.h"
#include "SpectatorController.h"
#include "WorldCameraTracker.h"
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <mutex>

// ============================================================================
// Internal State
// ============================================================================

static uintptr_t g_gameBase = 0;
static CameraConfig g_config;
static CameraState g_state = CameraState::Idle;
static std::mutex g_directorMutex;

// Current target
static int g_currentTargetHandle = 0;

// KillCam state
static KillCamPhase g_killCamPhase = KillCamPhase::Wait;
static int g_kcKillerHandle = 0;
static int g_kcVictimHandle = 0;
static DWORD g_kcStartTick = 0;

// Flag state
static int g_flagCarrierUS = 0;
static int g_flagCarrierVC = 0;
static DWORD g_flagAlternateTimer = 0;

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

// ============================================================================
// Entity helpers (same logic as FirstPersonCamera.cpp)
// ============================================================================

static void* CD_FindEntityByHandle(int handle) {
    void** playerTable = (void**)(g_gameBase + 0x7AE9C8);
    for (int i = 0; i < 64; i++) {
        if (!playerTable[i]) continue;
        if (*(int*)playerTable[i] == handle) {
            uintptr_t entity = *(uintptr_t*)((uintptr_t)playerTable[i] + 244);
            return entity ? (void*)entity : nullptr;
        }
    }
    return nullptr;
}

static void CD_GetEntityPos(void* entity, float* out) {
    out[0] = *(float*)((uintptr_t)entity + 0xD0);
    out[1] = *(float*)((uintptr_t)entity + 0xD4);
    out[2] = *(float*)((uintptr_t)entity + 0xD8);
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

static void EnterFollowPlayer(int targetHandle) {
    g_state = CameraState::FollowPlayer;
    g_currentTargetHandle = targetHandle;
    g_currentHoldStart = GetTickCount();
    SetSpectatorToPlayerId(targetHandle);
    WorldCameraTracker_SetTarget(targetHandle);
}

static void EnterKillCam(int killerHandle, int victimHandle) {
    g_state = CameraState::KillCam;
    g_killCamPhase = KillCamPhase::Wait;
    g_kcKillerHandle = killerHandle;
    g_kcVictimHandle = victimHandle;
    g_kcStartTick = GetTickCount();
    g_currentTargetHandle = killerHandle;

    SetSpectatorToPlayerId(killerHandle);
    WorldCameraTracker_SetTarget(killerHandle);

    std::cout << "[CameraDirector] KillCam started (killer=" << killerHandle
              << " victim=" << victimHandle << ")\n";
}

static void EnterFlagWatch(int carrierHandle) {
    g_state = CameraState::FlagWatch;
    g_currentTargetHandle = carrierHandle;
    SetSpectatorToPlayerId(carrierHandle);
    WorldCameraTracker_SetTarget(carrierHandle);
}

static void ExitKillCam() {
    g_state = CameraState::FollowPlayer;
    g_kcKillerHandle = 0;
    g_kcVictimHandle = 0;
    WorldCameraTracker_ClearTarget();
    g_lastEventTick = GetTickCount();
    std::cout << "[CameraDirector] KillCam ended\n";
}

static void EnterDrone() {
    // Get current camera position from followed player as starting point
    float startPos[3] = { 0.0f, 0.0f, 0.0f };
    void* entity = CD_FindEntityByHandle(g_currentTargetHandle);
    if (entity) {
        CD_GetEntityPos(entity, startPos);
        startPos[2] += 2.0f; // start slightly above player
    }
    DroneCamera_Activate(startPos);
    g_state = CameraState::Drone;
    g_currentHoldStart = GetTickCount();
    std::cout << "[CameraDirector] Drone mode activated (deficit scheduler)\n";
}

static void ExitDrone() {
    DroneCamera_Deactivate();
    g_state = CameraState::FollowPlayer;
    g_lastEventTick = GetTickCount();
    g_currentHoldStart = GetTickCount();
    std::cout << "[CameraDirector] Drone mode deactivated (deficit scheduler)\n";
}

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

// Compute deficit for each camera type and return the one with highest deficit
static CamBudgetType GetHighestDeficitType() {
    // FPV development: always use player camera
    return CAM_PLAYER;
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
        ExitKillCam();
    }
}

// ============================================================================
// Event Processing
// ============================================================================

void CameraDirector_OnKill(int killerHandle, int victimHandle) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();
    g_lastEventTick = now;

    // In drone mode: feed kill target to drone for dual-player framing
    if (g_state == CameraState::Drone) {
        DroneCamera_SetKillTarget(killerHandle, victimHandle);
        std::cout << "[CameraDirector] Drone: kill event -> framing killer=" << killerHandle
                  << " victim=" << victimHandle << "\n";
        return;
    }

    // Respect cooldown
    if (now - g_lastKillSwitch < (DWORD)(g_config.killCooldown * 1000)) {
        return;
    }

    // Don't interrupt flag watching
    if (g_state == CameraState::FlagWatch) {
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

    g_lastKillSwitch = now;

    if (cinematic) {
        EnterKillCam(killerHandle, victimHandle);
    } else {
        EnterFollowPlayer(killerHandle);
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

        // In drone mode: feed flag carrier to drone
        if (g_state == CameraState::Drone) {
            int target = 0;
            if (usCarrier != 0 && vcCarrier == 0) target = usCarrier;
            else if (vcCarrier != 0 && usCarrier == 0) target = vcCarrier;
            else target = usCarrier; // both — pick one
            DroneCamera_SetTarget(target);
            std::cout << "[CameraDirector] Drone: flag event -> tracking carrier=" << target << "\n";
            return;
        }

        // Pick which carrier to watch
        int target = 0;
        if (usCarrier != 0 && vcCarrier == 0) target = usCarrier;
        else if (vcCarrier != 0 && usCarrier == 0) target = vcCarrier;
        else {
            // Both flags taken — watch the one we're not currently on
            target = (g_currentTargetHandle == usCarrier) ? vcCarrier : usCarrier;
            g_flagAlternateTimer = GetTickCount();
        }

        // Only interrupt KillCam if it's interruptible
        if (g_state == CameraState::KillCam && !IsKillCamInterruptible()) {
            return;
        }

        EnterFlagWatch(target);
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
    // FPV dev: force player follow, no mode switching
    if (g_state != CameraState::FollowPlayer && g_state != CameraState::Idle) {
        if (g_state == CameraState::Drone) ExitDrone();
        g_state = CameraState::FollowPlayer;
    }
    return;  // skip all update logic
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
                    g_state = CameraState::FollowPlayer;
                    g_currentHoldStart = now;
                    WorldCameraTracker_ClearTarget();
                    std::cout << "[CameraDirector] Flag grace expired, returning to kills\n";
                    break;
                }
            }

            // Handle dual-flag alternation (5s toggle)
            if (g_flagCarrierUS != 0 && g_flagCarrierVC != 0) {
                float elapsed = (now - g_flagAlternateTimer) / 1000.0f;
                if (elapsed > 5.0f) {
                    int nextTarget = (g_currentTargetHandle == g_flagCarrierUS)
                                   ? g_flagCarrierVC : g_flagCarrierUS;
                    g_currentTargetHandle = nextTarget;
                    SetSpectatorToPlayerId(nextTarget);
                    WorldCameraTracker_SetTarget(nextTarget);
                    g_flagAlternateTimer = now;
                }
            }
            break;
        }

        case CameraState::Drone: {
            float holdSec = (now - g_currentHoldStart) / 1000.0f;

            // While in drone, periodically re-check area clearance
            if (holdSec >= g_config.camMinHoldDrone && !IsDroneSuitable()) {
                std::cout << "[CameraDirector] Drone exiting early: player moved indoors\n";
                ExitDrone();
                break;
            }

            // Deficit check: if drone has met its share and hold expired, switch
            if (holdSec >= g_config.camMinHoldDrone) {
                CamBudgetType needed = GetHighestDeficitType();
                if (needed != CAM_DRONE) {
                    ExitDrone();
                    if (needed == CAM_WORLD) {
                        WorldCameraTracker_SetPreference(0);
                    } else {
                        WorldCameraTracker_SetPreference(2);
                    }
                }
            }
            break;
        }

        case CameraState::FollowPlayer:
        case CameraState::Idle: {
            float holdSec = (now - g_currentHoldStart) / 1000.0f;
            int wcType = WorldCameraTracker_GetCurrentCamType();
            bool onWorldCam = (wcType == 0 || wcType == 1);
            bool onPlayerCam = (wcType == 2 || wcType == -1);

            float minHold = onWorldCam ? g_config.camMinHoldWorld : g_config.camMinHoldPlayer;

            if (holdSec >= minHold) {
                CamBudgetType needed = GetHighestDeficitType();

                if (needed == CAM_DRONE && IsDroneSuitable()) {
                    EnterDrone();
                } else if (needed == CAM_WORLD && onPlayerCam) {
                    WorldCameraTracker_SetPreference(0);
                    g_currentHoldStart = now;
                } else if (needed == CAM_PLAYER && onWorldCam) {
                    WorldCameraTracker_SetPreference(2);
                    g_currentHoldStart = now;
                }
                // If already on the needed type, keep going — hold resets on actual switch
            }
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

// ============================================================================
// Lifecycle
// ============================================================================

void InitCameraDirector(uintptr_t gameBase) {
    g_gameBase = gameBase;
    srand((unsigned int)time(nullptr));

    DroneCamera_Init(gameBase);
    LoadCameraDirectorConfig(g_config);
    g_lastEventTick = GetTickCount();
    g_lastUpdateTick = GetTickCount();
    g_currentHoldStart = GetTickCount();
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
