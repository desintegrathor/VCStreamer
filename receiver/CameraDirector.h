#pragma once
#include <Windows.h>
#include <atomic>

// ============================================================================
// Configuration — all tunables loaded from vcstreamer.ini
// ============================================================================

struct CameraConfig {
    // Distance-based cinematic thresholds
    float cinematicMinDistance      = 25.0f;
    float cinematicMaxDistance      = 50.0f;
    float cinematicMinChance       = 0.30f;
    float cinematicMaxChance       = 0.70f;

    // KillCam timing (seconds)
    float killCamWaitDuration      = 3.5f;
    float killCamTransitionDuration = 1.5f;
    float killCamAttachedDuration  = 4.0f;
    float killCamSlideHeight       = 0.8f;

    // Cooldowns (seconds)
    float killCooldown             = 5.0f;
    float flagLostGracePeriod      = 3.0f;

    // World camera
    float worldCamMaxDistance       = 18.0f;
    float worldCamSwitchCooldown   = 4.0f;
    float worldCamMaxHold          = 10.0f;
    float worldCamLOSPenalty       = 60.0f;
    float worldCamStickiness       = 5.0f;

    // 3PV smoothing
    float tpvMaxDistance           = 2.5f;
    float tpvYawSmoothFactor      = 0.01f;
    float tpvZoomInFactor          = 0.3f;
    float tpvZoomOutFactor         = 0.005f;

    // Debug
    bool debugMode                 = false;
};

// ============================================================================
// Camera State
// ============================================================================

enum class CameraState {
    Idle,
    FollowPlayer,
    KillCam,
    FlagWatch
};

enum class KillCamPhase {
    Wait,
    Transition,
    Attached
};

// ============================================================================
// Public API
// ============================================================================

// Lifecycle
void InitCameraDirector(uintptr_t gameBase);

// Event input (called by DelayManager when delayed actions fire)
void CameraDirector_OnKill(int killerHandle, int victimHandle);
void CameraDirector_OnFlagChanged(int usCarrier, int vcCarrier);

// Per-frame update (called from main loop)
void CameraDirector_Update();

// State query (called by Hooked_FillCamera to know what to render)
CameraState CameraDirector_GetState();
KillCamPhase CameraDirector_GetKillCamPhase();
int CameraDirector_GetTargetHandle();
int CameraDirector_GetKillCamKillerHandle();
int CameraDirector_GetKillCamVictimHandle();
float CameraDirector_GetKillCamElapsed();
const CameraConfig& CameraDirector_GetConfig();

// Config loading (implemented in DelayManager.cpp, follows existing INI pattern)
void LoadCameraDirectorConfig(CameraConfig& config);
