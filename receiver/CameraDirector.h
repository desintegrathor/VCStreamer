#pragma once
#include <Windows.h>
#include <atomic>

// ============================================================================
// Configuration — all tunables loaded from vcstreamer.ini
// ============================================================================

struct CameraConfig {
    // Distance-based cinematic thresholds
    float cinematicMinDistance      = 35.0f;
    float cinematicMaxDistance      = 65.0f;
    float cinematicMinChance       = 0.05f;
    float cinematicMaxChance       = 0.25f;

    // KillCam timing (seconds)
    float killCamWaitDuration      = 3.5f;
    float killCamTransitionDuration = 1.5f;
    float killCamAttachedDuration  = 4.0f;
    float killCamSlideHeight       = 0.8f;

    // Cooldowns (seconds)
    float killCooldown             = 15.0f;
    float flagLostGracePeriod      = 7.0f;

    // World camera
    float worldCamMaxDistance       = 22.0f;
    float worldCamSwitchCooldown   = 12.0f;
    float worldCamMaxHold          = 24.0f;
    float worldCamLOSPenalty       = 1000.0f;
    float worldCamStickiness       = 15.0f;

    // World camera auto-zoom
    float worldCamZoomStartDist    = 25.0f;
    float worldCamZoomMaxDist      = 50.0f;
    float worldCamZoomMaxFactor    = 0.4f;
    float worldCamZoomSmoothFactor = 0.05f;

    // 3PV smoothing
    float tpvMaxDistance           = 2.5f;
    float tpvYawSmoothFactor      = 0.01f;
    float tpvZoomInFactor          = 0.3f;
    float tpvZoomOutFactor         = 0.005f;

    // Drone camera
    float droneSpeed               = 8.0f;
    float droneAccel               = 12.0f;
    float droneDrag                = 2.5f;
    float droneMinWallDist         = 2.0f;
    float droneTurnRate            = 2.0f;
    float droneOrbitRadius         = 8.0f;
    float droneOrbitHeight         = 3.0f;
    float droneLookSmooth          = 0.08f;
    float droneHoldDuration        = 8.0f;
    float droneIdleTimeout         = 2.0f;

    // Camera budget shares (normalized at runtime)
    float camSharePlayer           = 0.12f;  // target share for player/FPV camera
    float camShareWorld            = 0.80f;  // target share for world cameras
    float camShareDrone            = 0.08f;  // target share for drone

    // Min hold times (seconds) before voluntary switch
    float camMinHoldPlayer         = 14.0f;
    float camMinHoldWorld          = 20.0f;
    float camMinHoldDrone          = 20.0f;

    // Drone area clearance threshold
    float droneMinAreaClearance    = 5.0f;   // avg clearance needed for drone activation

    // Drone pathfinding (Theta* + B-Spline)
    float droneGridResolution      = 1.0f;
    float droneClearanceBias       = 5.0f;
    float droneSlowClearance       = 4.0f;
    float dronePathReplanDist      = 5.0f;

    // Drone kill-frame & vantage points
    float droneKillFrameDuration    = 4.0f;
    float droneVantageHeight        = 6.0f;
    float droneVantageRadius        = 10.0f;
    float droneVantageRecomputeDist = 5.0f;

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
    FlagWatch,
    Drone
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
bool CameraDirector_ShouldUseFpv();

// Config loading (implemented in DelayManager.cpp, follows existing INI pattern)
void LoadCameraDirectorConfig(CameraConfig& config);
