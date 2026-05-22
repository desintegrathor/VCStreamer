#define _CRT_SECURE_NO_WARNINGS
#include "DroneCamera.h"
#include "CameraDirector.h"
#include "DiagnosticsLog.h"
#include "OctCollision.h"
#include "PathGrid.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// ============================================================================
// Internal State
// ============================================================================

static uintptr_t g_gameBase = 0;
static bool g_active = false;

static void DroneLog(const char* fmt, ...) {
    if (!DiagnosticsLog_IsEnabled()) return;

    FILE* file = fopen("receiver_debug.log", "a");
    if (!file) return;

    va_list args;
    va_start(args, fmt);
    DiagnosticsLog_Write(file, fmt, args);
    va_end(args);

    fclose(file);
}

static float g_pos[3] = {};
static float g_vel[3] = {};
static float g_pitch = 0.0f;
static float g_yaw = 0.0f;

// Current target the drone is flying toward
static int g_targetHandle = 0;

// Player visibility: LOS from drone to player head
static bool g_canSeePlayer = false;

// Smoothing state for avoidance and look-at stabilization
static float g_prevAvoidForce[3] = {};         // EMA of avoidance force
static float g_prevAvoidFwd[2] = {0.0f, 1.0f}; // smoothed avoidance forward direction
static float g_canSeePlayerTimer = 0.0f;        // hysteresis timer for visibility
static bool  g_canSeePlayerStable = false;       // debounced visibility state

// Kill-frame state
static int g_victimHandle = 0;
static bool g_killFrameActive = false;
static float g_killFrameTimer = 0.0f;
static float g_viewpointPos[3] = {};
static float g_lookAtPos[3] = {};

// Vantage point state (replaces orbit)
static float g_lastVantagePlayerPos[3] = {};
static float g_vantageRecomputeTimer = 0.0f;
static bool g_vantageValid = false;

// ============================================================================
// Pathfinding State (Theta* + B-Spline)
// ============================================================================

static constexpr int MAX_WAYPOINTS = 64;

static float g_waypoints[MAX_WAYPOINTS][3];
static int g_waypointCount = 0;

static float g_goalPos[3] = {};
static bool g_pathReady = false;
static float g_stuckTimer = 0.0f;
static float g_losCheckTimer = 0.0f;
static bool g_usingDirectSteering = true;

// Spline follow state
static float g_splineU = 0.0f;          // current parameter along spline (0..1)
static bool g_splineReady = false;
static float g_splineDistTraveled = 0.0f;

// Grid build state (amortized across frames)
static bool g_gridBuildPending = false;

// ============================================================================
// Helpers
// ============================================================================

static constexpr float PI = 3.14159265f;
static constexpr float TWO_PI = 6.28318530f;

static float NormalizeAngle(float a) {
    while (a > PI) a -= TWO_PI;
    while (a < -PI) a += TWO_PI;
    return a;
}

static float LerpAngle(float from, float to, float t) {
    float diff = NormalizeAngle(to - from);
    return from + diff * t;
}

static float Vec3Length(const float* v) {
    return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

static void Vec3Normalize(float* v) {
    float len = Vec3Length(v);
    if (len > 0.0001f) {
        v[0] /= len; v[1] /= len; v[2] /= len;
    }
}

static float Vec3Dot(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static float Vec3Dist(const float* a, const float* b) {
    float d[3] = { a[0]-b[0], a[1]-b[1], a[2]-b[2] };
    return Vec3Length(d);
}

static float RandFloat() {
    return (float)rand() / (float)RAND_MAX;
}

// ============================================================================
// Entity access
// ============================================================================

static void* FindEntityByHandle(int handle) {
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

static void GetEntityPos(void* entity, float* out) {
    out[0] = *(float*)((uintptr_t)entity + 0xD0);
    out[1] = *(float*)((uintptr_t)entity + 0xD4);
    out[2] = *(float*)((uintptr_t)entity + 0xD8);
}

// ============================================================================
// Vantage Point Scoring
// ============================================================================

// Find a viewpoint that can see both killer and victim from an elevated angle
static bool FindKillViewpoint(const float* killerPos, const float* victimPos, const CameraConfig& cfg) {
    float mid[3] = {
        (killerPos[0] + victimPos[0]) * 0.5f,
        (killerPos[1] + victimPos[1]) * 0.5f,
        (killerPos[2] + victimPos[2]) * 0.5f
    };

    float dx = killerPos[0] - victimPos[0];
    float dy = killerPos[1] - victimPos[1];
    float dz = killerPos[2] - victimPos[2];
    float separation = sqrtf(dx*dx + dy*dy + dz*dz);
    float camDist = separation * 0.8f;
    if (camDist < 6.0f) camDist = 6.0f;

    // Kill-victim direction (normalized XY)
    float kvDir[3] = { dx, dy, 0.0f };
    float kvLen = sqrtf(kvDir[0]*kvDir[0] + kvDir[1]*kvDir[1]);
    if (kvLen > 0.01f) { kvDir[0] /= kvLen; kvDir[1] /= kvLen; }

    float killerHead[3] = { killerPos[0], killerPos[1], killerPos[2] + 1.3f };
    float victimHead[3] = { victimPos[0], victimPos[1], victimPos[2] + 1.3f };

    float bestScore = -1e30f;
    float bestPos[3] = { mid[0], mid[1], mid[2] + 8.0f };
    bool foundDual = false;
    float bestSingleScore = -1e30f;
    float bestSinglePos[3] = {};

    float heights[3] = { mid[2] + 4.0f, mid[2] + 7.0f, mid[2] + 10.0f };

    for (int h = 0; h < 3; h++) {
        for (int a = 0; a < 12; a++) {
            float angle = (float)a * (TWO_PI / 12.0f);
            float cx = mid[0] + camDist * sinf(angle);
            float cy = mid[1] + camDist * cosf(angle);
            float cz = heights[h];
            float candidate[3] = { cx, cy, cz };

            bool losKiller = !OctCollision_IsLoaded() || OctCollision_LineOfSight(candidate, killerHead);
            bool losVictim = !OctCollision_IsLoaded() || OctCollision_LineOfSight(candidate, victimHead);

            if (!losKiller && !losVictim) continue;

            float clearance = PathGrid_IsReady() ? PathGrid_GetClearance(cx, cy, cz) : 5.0f;
            float heightBonus = (cz - mid[2]) * 1.5f;
            float proxPenalty = -Vec3Dist(candidate, g_pos) * 0.3f;

            // Perpendicularity: prefer side views
            float toMid[3] = { cx - mid[0], cy - mid[1], 0.0f };
            float toMidLen = sqrtf(toMid[0]*toMid[0] + toMid[1]*toMid[1]);
            float perpScore = 0.0f;
            if (toMidLen > 0.01f && kvLen > 0.01f) {
                float dot = fabsf((toMid[0]/toMidLen) * kvDir[0] + (toMid[1]/toMidLen) * kvDir[1]);
                perpScore = (1.0f - dot) * 3.0f;
            }

            float score = clearance * 2.0f + heightBonus + proxPenalty + perpScore;

            if (losKiller && losVictim) {
                if (score > bestScore) {
                    bestScore = score;
                    bestPos[0] = cx; bestPos[1] = cy; bestPos[2] = cz;
                    foundDual = true;
                }
            } else if (losKiller) {
                if (score > bestSingleScore) {
                    bestSingleScore = score;
                    bestSinglePos[0] = cx; bestSinglePos[1] = cy; bestSinglePos[2] = cz;
                }
            }
        }
    }

    if (!foundDual && bestSingleScore > -1e29f) {
        bestPos[0] = bestSinglePos[0];
        bestPos[1] = bestSinglePos[1];
        bestPos[2] = bestSinglePos[2];
    }

    g_viewpointPos[0] = bestPos[0];
    g_viewpointPos[1] = bestPos[1];
    g_viewpointPos[2] = bestPos[2];
    g_lookAtPos[0] = (killerHead[0] + victimHead[0]) * 0.5f;
    g_lookAtPos[1] = (killerHead[1] + victimHead[1]) * 0.5f;
    g_lookAtPos[2] = (killerHead[2] + victimHead[2]) * 0.5f;

    DroneLog("[DroneCamera] Kill viewpoint: (%.2f, %.2f, %.2f) dualLOS=%d score=%.2f\n",
             bestPos[0], bestPos[1], bestPos[2], foundDual ? 1 : 0,
             foundDual ? bestScore : bestSingleScore);

    return foundDual || bestSingleScore > -1e29f;
}

// Find an elevated vantage point for normal (non-kill) tracking
static void FindVantagePoint(const float* playerPos, const CameraConfig& cfg) {
    float playerHead[3] = { playerPos[0], playerPos[1], playerPos[2] + 1.3f };
    float radius = cfg.droneVantageRadius;

    float bestScore = -1e30f;
    float bestPos[3] = { playerPos[0], playerPos[1], playerPos[2] + cfg.droneVantageHeight };

    float heights[2] = { playerPos[2] + 5.0f, playerPos[2] + 8.0f };

    for (int h = 0; h < 2; h++) {
        for (int a = 0; a < 8; a++) {
            float angle = (float)a * (TWO_PI / 8.0f);
            float cx = playerPos[0] + radius * sinf(angle);
            float cy = playerPos[1] + radius * cosf(angle);
            float cz = heights[h];
            float candidate[3] = { cx, cy, cz };

            bool los = !OctCollision_IsLoaded() || OctCollision_LineOfSight(candidate, playerHead);
            if (!los) continue;

            float clearance = PathGrid_IsReady() ? PathGrid_GetClearance(cx, cy, cz) : 5.0f;
            float heightBonus = (cz - playerPos[2]) * 1.5f;
            float proxPenalty = -Vec3Dist(candidate, g_pos) * 0.3f;

            float score = clearance * 2.0f + heightBonus + proxPenalty;
            if (score > bestScore) {
                bestScore = score;
                bestPos[0] = cx; bestPos[1] = cy; bestPos[2] = cz;
            }
        }
    }

    g_viewpointPos[0] = bestPos[0];
    g_viewpointPos[1] = bestPos[1];
    g_viewpointPos[2] = bestPos[2];
    g_lastVantagePlayerPos[0] = playerPos[0];
    g_lastVantagePlayerPos[1] = playerPos[1];
    g_lastVantagePlayerPos[2] = playerPos[2];
    g_vantageRecomputeTimer = 0.0f;
    g_vantageValid = true;
}

// ============================================================================
// Obstacle Avoidance (safety layer — rarely needed with PathGrid)
// ============================================================================

static void ComputeAvoidance(float* outForce, const CameraConfig& cfg, bool pathFollowing) {
    outForce[0] = outForce[1] = outForce[2] = 0.0f;

    if (!OctCollision_IsLoaded()) return;

    float minDist = cfg.droneMinWallDist;

    float velSpeed = sqrtf(g_vel[0]*g_vel[0] + g_vel[1]*g_vel[1]);
    float rawFwdX, rawFwdY;
    if (velSpeed > 1.0f) {
        rawFwdX = g_vel[0] / velSpeed;
        rawFwdY = g_vel[1] / velSpeed;
    } else {
        rawFwdX = sinf(g_yaw);
        rawFwdY = cosf(g_yaw);
    }
    float fwdAlpha = 0.3f;
    float fwdX = g_prevAvoidFwd[0] + fwdAlpha * (rawFwdX - g_prevAvoidFwd[0]);
    float fwdY = g_prevAvoidFwd[1] + fwdAlpha * (rawFwdY - g_prevAvoidFwd[1]);
    float fwdLen = sqrtf(fwdX*fwdX + fwdY*fwdY);
    if (fwdLen > 0.01f) { fwdX /= fwdLen; fwdY /= fwdLen; }
    g_prevAvoidFwd[0] = fwdX;
    g_prevAvoidFwd[1] = fwdY;

    float spread = 0.5f;

    int rayCount = pathFollowing ? 3 : 5;

    float rays[5][3];
    rays[0][0] = fwdX; rays[0][1] = fwdY; rays[0][2] = 0.0f;

    if (pathFollowing) {
        rays[1][0] = fwdX * 0.866f; rays[1][1] = fwdY * 0.866f; rays[1][2] = spread;
        rays[2][0] = fwdX * 0.866f; rays[2][1] = fwdY * 0.866f; rays[2][2] = -spread;
    } else {
        rays[1][0] = fwdX * 0.866f - fwdY * spread;
        rays[1][1] = fwdY * 0.866f + fwdX * spread;
        rays[1][2] = 0.0f;
        rays[2][0] = fwdX * 0.866f + fwdY * spread;
        rays[2][1] = fwdY * 0.866f - fwdX * spread;
        rays[2][2] = 0.0f;
        rays[3][0] = fwdX * 0.866f; rays[3][1] = fwdY * 0.866f; rays[3][2] = spread;
        rays[4][0] = fwdX * 0.866f; rays[4][1] = fwdY * 0.866f; rays[4][2] = -spread;
    }

    for (int i = 0; i < rayCount; i++) {
        Vec3Normalize(rays[i]);

        float hitNormal[3] = {};
        float dist = OctCollision_Raycast(g_pos, rays[i], hitNormal);

        if (dist < minDist) {
            float weight = (minDist - dist) / minDist;

            if (Vec3Length(hitNormal) > 0.1f) {
                outForce[0] += hitNormal[0] * weight * 12.0f;
                outForce[1] += hitNormal[1] * weight * 12.0f;
                outForce[2] += hitNormal[2] * weight * 12.0f;
            } else {
                outForce[0] -= rays[i][0] * weight * 12.0f;
                outForce[1] -= rays[i][1] * weight * 12.0f;
                outForce[2] -= rays[i][2] * weight * 12.0f;
            }
        }
    }

    float smooth = 0.4f;
    outForce[0] = g_prevAvoidForce[0] + smooth * (outForce[0] - g_prevAvoidForce[0]);
    outForce[1] = g_prevAvoidForce[1] + smooth * (outForce[1] - g_prevAvoidForce[1]);
    outForce[2] = g_prevAvoidForce[2] + smooth * (outForce[2] - g_prevAvoidForce[2]);
    g_prevAvoidForce[0] = outForce[0];
    g_prevAvoidForce[1] = outForce[1];
    g_prevAvoidForce[2] = outForce[2];
}

// ============================================================================
// Path Building (Theta* + B-Spline)
// ============================================================================

static void ClearPath() {
    g_waypointCount = 0;
    g_pathReady = false;
    g_splineReady = false;
    g_splineU = 0.0f;
    g_splineDistTraveled = 0.0f;
}

static bool BuildPath(const float* startPos, const float* goalPos, const CameraConfig& cfg) {
    g_goalPos[0] = goalPos[0];
    g_goalPos[1] = goalPos[1];
    g_goalPos[2] = goalPos[2];

    if (!PathGrid_IsReady()) {
        // Grid not built yet — trigger build
        if (!g_gridBuildPending) {
            g_gridBuildPending = true;
            PathGrid_Build(cfg.droneGridResolution);
            g_gridBuildPending = false;
        }
        if (!PathGrid_IsReady()) return false;
    }

    // Run Theta* search
    g_waypointCount = PathGrid_FindPath(startPos, goalPos, g_waypoints, MAX_WAYPOINTS,
                                         cfg.droneClearanceBias, cfg.droneMinWallDist);

    if (g_waypointCount < 2) {
        // Single waypoint or no path — not useful
        ClearPath();
        return false;
    }

    // Fit B-spline with collision validation
    g_splineReady = PathGrid_FitSpline(g_waypoints, g_waypointCount, cfg.droneMinWallDist);
    if (!g_splineReady) {
        ClearPath();
        return false;
    }

    g_splineU = 0.0f;
    g_splineDistTraveled = 0.0f;
    g_pathReady = true;

    DroneLog("[DroneCamera] Path ready: %d waypoints, %.2f units\n",
             g_waypointCount, PathGrid_GetSplineLength());
    return true;
}

// ============================================================================
// Spline Following — compute desired velocity along B-spline
// ============================================================================

static void ComputeSplineFollowVelocity(float* desiredVel, float speed, float dt,
                                         const CameraConfig& cfg) {
    if (!g_splineReady || !g_pathReady) {
        desiredVel[0] = desiredVel[1] = desiredVel[2] = 0.0f;
        return;
    }

    float splineLen = PathGrid_GetSplineLength();
    if (splineLen < 0.1f) {
        g_pathReady = false;
        desiredVel[0] = desiredVel[1] = desiredVel[2] = 0.0f;
        return;
    }

    // Adaptive speed: slow down in tight sections
    float clearance = PathGrid_GetSplineClearance(g_splineU);
    float speedMul = 1.0f;
    if (clearance < cfg.droneSlowClearance) {
        speedMul = clearance / cfg.droneSlowClearance;
        if (speedMul < 0.3f) speedMul = 0.3f;
    }
    float actualSpeed = speed * speedMul;

    // Advance spline parameter based on speed
    float uStep = (actualSpeed * dt) / splineLen;
    float carrotU = g_splineU + uStep * 3.0f; // look ahead 3 steps
    if (carrotU > 1.0f) carrotU = 1.0f;

    float carrot[3];
    PathGrid_EvalSpline(carrotU, carrot);

    // Steer toward carrot point
    float toCarrot[3] = { carrot[0]-g_pos[0], carrot[1]-g_pos[1], carrot[2]-g_pos[2] };
    float dist = Vec3Length(toCarrot);

    if (dist > 0.01f) {
        desiredVel[0] = toCarrot[0] / dist * actualSpeed;
        desiredVel[1] = toCarrot[1] / dist * actualSpeed;
        desiredVel[2] = toCarrot[2] / dist * actualSpeed;
    } else {
        desiredVel[0] = desiredVel[1] = desiredVel[2] = 0.0f;
    }

    // Advance spline parameter based on actual position
    // Find closest u to current position by checking if we've passed the current carrot
    float currentTarget[3];
    PathGrid_EvalSpline(g_splineU + uStep, currentTarget);
    float distToNext = Vec3Dist(g_pos, currentTarget);
    if (distToNext < 1.5f || dist < 1.0f) {
        g_splineU += uStep;
    }

    // Check if at end of spline
    if (g_splineU >= 0.95f) {
        float endPos[3];
        PathGrid_EvalSpline(1.0f, endPos);
        if (Vec3Dist(g_pos, endPos) < 2.0f) {
            g_pathReady = false;
            g_splineReady = false;
            g_usingDirectSteering = true;
            DroneLog("[DroneCamera] Spline follow complete\n");
        }
    }
}

// ============================================================================
// Replan triggers
// ============================================================================

static void CheckReplanTriggers(float dt, const float* orbitTarget, const CameraConfig& cfg) {
    // Target moved significantly from stored goal
    if (g_pathReady) {
        float goalDist = Vec3Dist(orbitTarget, g_goalPos);
        if (goalDist > cfg.dronePathReplanDist) {
            DroneLog("[DroneCamera] Replan: target moved %.2f units\n", goalDist);
            ClearPath();
            g_usingDirectSteering = true;
        }
    }

    // Stuck detection
    float speed = Vec3Length(g_vel);
    if (speed < 0.3f && g_pathReady) {
        g_stuckTimer += dt;
        if (g_stuckTimer > 1.5f) {
            DroneLog("[DroneCamera] Replan: stuck (speed=%.2f)\n", speed);
            ClearPath();
            g_stuckTimer = 0.0f;
            g_usingDirectSteering = true;
            g_vel[0] += (RandFloat() - 0.5f) * 3.0f;
            g_vel[1] += (RandFloat() - 0.5f) * 3.0f;
            g_vel[2] += (RandFloat() - 0.5f) * 1.0f;
        }
    } else {
        g_stuckTimer = 0.0f;
    }
}

// ============================================================================
// Public API
// ============================================================================

void DroneCamera_Init(uintptr_t gameBase) {
    g_gameBase = gameBase;
}

void DroneCamera_Activate(const float startPos[3]) {
    g_pos[0] = startPos[0];
    g_pos[1] = startPos[1];
    g_pos[2] = startPos[2];
    g_vel[0] = g_vel[1] = g_vel[2] = 0.0f;
    g_pitch = 0.0f;
    g_yaw = 0.0f;
    g_targetHandle = 0;
    ClearPath();
    g_usingDirectSteering = true;
    g_stuckTimer = 0.0f;
    g_losCheckTimer = 0.0f;
    g_canSeePlayer = false;
    g_prevAvoidForce[0] = g_prevAvoidForce[1] = g_prevAvoidForce[2] = 0.0f;
    g_prevAvoidFwd[0] = 0.0f; g_prevAvoidFwd[1] = 1.0f;
    g_canSeePlayerTimer = 0.0f;
    g_canSeePlayerStable = false;
    g_gridBuildPending = false;
    g_killFrameActive = false;
    g_killFrameTimer = 0.0f;
    g_victimHandle = 0;
    g_vantageValid = false;
    g_vantageRecomputeTimer = 0.0f;

    // Build clearance grid if not already built
    if (!PathGrid_IsReady() && OctCollision_IsLoaded()) {
        const CameraConfig& cfg = CameraDirector_GetConfig();
        PathGrid_Build(cfg.droneGridResolution);
    }

    g_active = true;
    DroneLog("[DroneCamera] Activated at (%.2f, %.2f, %.2f)\n",
             startPos[0], startPos[1], startPos[2]);
}

void DroneCamera_Deactivate() {
    if (g_active) {
        g_active = false;
        ClearPath();
        DroneLog("[DroneCamera] Deactivated\n");
    }
}

bool DroneCamera_IsActive() {
    return g_active;
}

void DroneCamera_SetTarget(int playerHandle) {
    if (playerHandle != g_targetHandle && playerHandle != 0) {
        g_targetHandle = playerHandle;
        g_killFrameActive = false;
        g_victimHandle = 0;
        g_vantageValid = false;
        ClearPath();
        g_usingDirectSteering = true;
        DroneLog("[DroneCamera] New target: %d\n", playerHandle);
    }
}

void DroneCamera_SetKillTarget(int killerHandle, int victimHandle) {
    g_targetHandle = killerHandle;
    g_victimHandle = victimHandle;
    ClearPath();
    g_usingDirectSteering = true;

    // Try to find killer and victim positions for viewpoint computation
    void* killerEntity = FindEntityByHandle(killerHandle);
    void* victimEntity = FindEntityByHandle(victimHandle);

    const CameraConfig& cfg = CameraDirector_GetConfig();

    if (killerEntity && victimEntity) {
        float kp[3], vp[3];
        GetEntityPos(killerEntity, kp);
        GetEntityPos(victimEntity, vp);

        if (FindKillViewpoint(kp, vp, cfg)) {
            g_killFrameActive = true;
            g_killFrameTimer = cfg.droneKillFrameDuration;
        } else {
            // Fallback: just track killer with vantage point
            g_killFrameActive = false;
            g_vantageValid = false;
        }
    } else if (killerEntity) {
        // No victim entity — just track killer
        g_killFrameActive = false;
        g_vantageValid = false;
    }

    DroneLog("[DroneCamera] Kill target: killer=%d victim=%d killFrame=%d\n",
             killerHandle, victimHandle, g_killFrameActive ? 1 : 0);
}

int DroneCamera_GetTargetHandle() {
    return g_targetHandle;
}

void DroneCamera_GetCameraState(float* outPos, float* outPitch, float* outYaw) {
    outPos[0] = g_pos[0];
    outPos[1] = g_pos[1];
    outPos[2] = g_pos[2];
    *outPitch = g_pitch;
    *outYaw = g_yaw;
}

void DroneCamera_Update(float dt) {
    if (!g_active || dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;

    const CameraConfig& cfg = CameraDirector_GetConfig();

    // Target ownership belongs to CameraDirector. If the current target is
    // unavailable, hold position until the director commits a fallback.
    void* targetEntity = (g_targetHandle != 0) ? FindEntityByHandle(g_targetHandle) : nullptr;
    if (!targetEntity) {
        return;
    }

    // Check kill-frame expiry
    if (g_killFrameActive) {
        g_killFrameTimer -= dt;
        void* victimEntity = (g_victimHandle != 0) ? FindEntityByHandle(g_victimHandle) : nullptr;
        if (g_killFrameTimer <= 0.0f || !victimEntity) {
            g_killFrameActive = false;
            g_victimHandle = 0;
            g_vantageValid = false; // force vantage recompute
            DroneLog("[DroneCamera] Kill frame ended\n");
        }
    }

    // Compute desired velocity
    float desiredVel[3] = { 0.0f, 0.0f, 0.0f };
    float playerPos[3] = {};
    bool isFollowingPath = false;
    float lookTarget[3] = {};
    bool hasLookTarget = false;

    if (targetEntity) {
        GetEntityPos(targetEntity, playerPos);

        // Check if drone can actually see the player (head height)
        float playerHead[3] = { playerPos[0], playerPos[1], playerPos[2] + 1.3f };
        g_canSeePlayer = !OctCollision_IsLoaded() || OctCollision_LineOfSight(g_pos, playerHead);

        // Hysteresis for look-at stability
        if (g_canSeePlayer) {
            g_canSeePlayerTimer += dt;
            if (g_canSeePlayerTimer > 0.3f) g_canSeePlayerStable = true;
        } else {
            g_canSeePlayerTimer -= dt;
            if (g_canSeePlayerTimer < -0.3f) g_canSeePlayerStable = false;
        }
        g_canSeePlayerTimer = fmaxf(-0.5f, fminf(0.5f, g_canSeePlayerTimer));

        // Determine viewpoint destination
        float destPos[3];

        if (g_killFrameActive) {
            // Kill-frame mode: fly to the computed kill viewpoint
            destPos[0] = g_viewpointPos[0];
            destPos[1] = g_viewpointPos[1];
            destPos[2] = g_viewpointPos[2];

            // Update look-at to current midpoint of killer+victim heads
            void* victimEntity = FindEntityByHandle(g_victimHandle);
            if (victimEntity) {
                float vp[3];
                GetEntityPos(victimEntity, vp);
                g_lookAtPos[0] = (playerPos[0] + vp[0]) * 0.5f;
                g_lookAtPos[1] = (playerPos[1] + vp[1]) * 0.5f;
                g_lookAtPos[2] = ((playerPos[2] + 1.3f) + (vp[2] + 1.3f)) * 0.5f;
            }
            lookTarget[0] = g_lookAtPos[0];
            lookTarget[1] = g_lookAtPos[1];
            lookTarget[2] = g_lookAtPos[2];
            hasLookTarget = true;
        } else {
            // Normal vantage tracking: find elevated position with LOS
            g_vantageRecomputeTimer += dt;
            float playerMoved = Vec3Dist(playerPos, g_lastVantagePlayerPos);
            if (!g_vantageValid || playerMoved > cfg.droneVantageRecomputeDist || g_vantageRecomputeTimer > 3.0f) {
                FindVantagePoint(playerPos, cfg);
            }
            destPos[0] = g_viewpointPos[0];
            destPos[1] = g_viewpointPos[1];
            destPos[2] = g_viewpointPos[2];
            lookTarget[0] = playerHead[0];
            lookTarget[1] = playerHead[1];
            lookTarget[2] = playerHead[2];
            hasLookTarget = true;
        }

        // Navigate to destPos using LOS check → direct steering or pathfinding
        bool losToDest = !OctCollision_IsLoaded() || OctCollision_LineOfSight(g_pos, destPos);

        if (losToDest && g_pathReady) {
            ClearPath();
            g_usingDirectSteering = true;
        }

        if (losToDest) {
            g_usingDirectSteering = true;

            float toDest[3] = { destPos[0] - g_pos[0], destPos[1] - g_pos[1], destPos[2] - g_pos[2] };
            float dist = Vec3Length(toDest);

            if (dist > 0.01f) {
                float speed = cfg.droneSpeed;
                if (dist < 3.0f) speed *= dist / 3.0f;

                desiredVel[0] = toDest[0] / dist * speed;
                desiredVel[1] = toDest[1] / dist * speed;
                desiredVel[2] = toDest[2] / dist * speed;
            }
        } else {
            g_usingDirectSteering = false;

            if (g_pathReady && g_splineReady) {
                ComputeSplineFollowVelocity(desiredVel, cfg.droneSpeed, dt, cfg);
                isFollowingPath = true;
            } else {
                bool built = BuildPath(g_pos, destPos, cfg);
                if (built) {
                    DroneLog("[DroneCamera] Theta* path built to vantage point\n");
                }

                float toDest[3] = { destPos[0] - g_pos[0], destPos[1] - g_pos[1], destPos[2] - g_pos[2] };
                float dist = Vec3Length(toDest);
                if (dist > 0.01f) {
                    float speed = cfg.droneSpeed * 0.2f;
                    desiredVel[0] = toDest[0] / dist * speed;
                    desiredVel[1] = toDest[1] / dist * speed;
                    desiredVel[2] = toDest[2] / dist * speed;
                }
            }

            CheckReplanTriggers(dt, destPos, cfg);
        }
    } else {
        // No target: drift forward gently
        desiredVel[0] = sinf(g_yaw) * cfg.droneSpeed * 0.3f;
        desiredVel[1] = cosf(g_yaw) * cfg.droneSpeed * 0.3f;
        desiredVel[2] = 0.0f;
    }

    // Obstacle avoidance (safety layer)
    float avoidForce[3];
    ComputeAvoidance(avoidForce, cfg, isFollowingPath);
    desiredVel[0] += avoidForce[0];
    desiredVel[1] += avoidForce[1];
    desiredVel[2] += avoidForce[2];

    // Physics integration
    float accel[3];
    accel[0] = (desiredVel[0] - g_vel[0]) * cfg.droneAccel;
    accel[1] = (desiredVel[1] - g_vel[1]) * cfg.droneAccel;
    accel[2] = (desiredVel[2] - g_vel[2]) * cfg.droneAccel;

    g_vel[0] += accel[0] * dt;
    g_vel[1] += accel[1] * dt;
    g_vel[2] += accel[2] * dt;

    float drag = 1.0f - cfg.droneDrag * dt;
    if (drag < 0.0f) drag = 0.0f;
    g_vel[0] *= drag;
    g_vel[1] *= drag;
    g_vel[2] *= drag;

    // Clamp speed
    float speed = Vec3Length(g_vel);
    if (speed > cfg.droneSpeed) {
        float scale = cfg.droneSpeed / speed;
        g_vel[0] *= scale;
        g_vel[1] *= scale;
        g_vel[2] *= scale;
    }

    // Position validation (final guard against wall penetration)
    float newPos[3];
    newPos[0] = g_pos[0] + g_vel[0] * dt;
    newPos[1] = g_pos[1] + g_vel[1] * dt;
    newPos[2] = g_pos[2] + g_vel[2] * dt;

    if (OctCollision_IsLoaded()) {
        float moveDir[3] = { newPos[0] - g_pos[0], newPos[1] - g_pos[1], newPos[2] - g_pos[2] };
        float moveDist = Vec3Length(moveDir);
        if (moveDist > 0.001f) {
            moveDir[0] /= moveDist;
            moveDir[1] /= moveDist;
            moveDir[2] /= moveDist;

            float hitDist = OctCollision_Raycast(g_pos, moveDir, nullptr);
            if (hitDist < moveDist + 0.3f) {
                float safeDist = hitDist - 0.3f;
                if (safeDist < 0.0f) safeDist = 0.0f;
                newPos[0] = g_pos[0] + moveDir[0] * safeDist;
                newPos[1] = g_pos[1] + moveDir[1] * safeDist;
                newPos[2] = g_pos[2] + moveDir[2] * safeDist;
                float velDot = Vec3Dot(g_vel, moveDir);
                if (velDot > 0.0f) {
                    g_vel[0] -= moveDir[0] * velDot;
                    g_vel[1] -= moveDir[1] * velDot;
                    g_vel[2] -= moveDir[2] * velDot;
                }
            }
        }
    }

    g_pos[0] = newPos[0];
    g_pos[1] = newPos[1];
    g_pos[2] = newPos[2];

    // Look-at smoothing
    if (hasLookTarget) {
        float targetX = lookTarget[0] - g_pos[0];
        float targetY = lookTarget[1] - g_pos[1];
        float targetZ = lookTarget[2] - g_pos[2];

        float horizDist = sqrtf(targetX*targetX + targetY*targetY);
        if (horizDist > 0.01f) {
            float targetYaw = atan2f(-targetX, targetY);
            float targetPitch = -atan2f(targetZ, horizDist);

            g_yaw = LerpAngle(g_yaw, targetYaw, cfg.droneLookSmooth);
            g_pitch = g_pitch + (targetPitch - g_pitch) * cfg.droneLookSmooth;
        }
    } else {
        // No target: look in movement direction
        float horizSpeed = sqrtf(g_vel[0]*g_vel[0] + g_vel[1]*g_vel[1]);
        if (horizSpeed > 0.5f) {
            float targetYaw = atan2f(-g_vel[0], g_vel[1]);
            g_yaw = LerpAngle(g_yaw, targetYaw, cfg.droneLookSmooth);
        }
        g_pitch = g_pitch + (0.0f - g_pitch) * cfg.droneLookSmooth;
    }
}
