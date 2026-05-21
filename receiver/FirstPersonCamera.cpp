#define _CRT_SECURE_NO_WARNINGS
#include "FirstPersonCamera.h"
#include "CameraDirector.h"
#include "DroneCamera.h"
#include "DelayManager.h"
#include "WorldCameraTracker.h"
#include "SpectatedPlayerData.h"
#include "FpvViewmodel.h"
#include "RealtimeHook.h"
#include "OctCollision.h"
#include "PathGrid.h"
#include "minhook/MinHook.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

// ============================================================================
// Constants
// ============================================================================

constexpr int SPECTATOR_MODE_FREE = 2;

// Game.dll offsets (IDA addresses minus IDA base 0xD40000)
constexpr uintptr_t OFFSET_FILL_CAMERA = 0x147650;
constexpr uintptr_t OFFSET_GET_CHEST_POS = 0x18BCB0;
constexpr uintptr_t OFFSET_SET_CAM_ROTATION_IMPORT = 0x1E7718;
constexpr uintptr_t OFFSET_GET_BONE_LOC_IMPORT = 0x1E7E14;
constexpr uintptr_t OFFSET_GET_EYE_POS_IMPORT = 0x1E7B04;
constexpr uintptr_t OFFSET_GET_HAND_FIRE_IMPORT = 0x1E7B0C;
constexpr uintptr_t OFFSET_CALC_FOLLOW_CAM = 0x12E570;

// Structure offsets
constexpr int SPECTATOR_CTRL_MODE = 0x00;
constexpr int SPECTATOR_CTRL_CLIENT = 0x52C;
constexpr int CLIENT_PLAYER_ENTITY = 0xF4;
constexpr int PLAYER_SKELETON_PTR = 0x378;

// Bone IDs (runtime ISKE bone-array indices). BONE_HEAD=23 is canonical;
// ISKE hierarchy (Hips > Chest > Neck > Head) has bones ordered parent-first,
// so Neck=22 and Chest=21 (torso).
constexpr unsigned int BONE_HEAD  = 0x17;
constexpr unsigned int BONE_NECK  = 0x16;
constexpr unsigned int BONE_CHEST = 0x15;

constexpr float PI_F = 3.14159265f;
constexpr float YAW_OFFSET_3PV = PI_F - 0.35f;
constexpr float MAX_PITCH_3PV = 0.175f;

// NOD struct offsets — verified from NOD_FindByNameAndType @ 0xf429b0:
//   +8  name[], +0x58 flags (bit 0 = hidden, bit 3 = has_children),
//   +0x68 child_count (DWORD), +0x6C children[] (void** array).
// Note: Blender_vietcong docs claim count is float @ +0x6C; that's outdated.
constexpr int NOD_NAME_OFF        = 0x08;
constexpr int NOD_FLAGS_OFF       = 0x58;
constexpr int NOD_CHILD_COUNT_OFF = 0x68;
constexpr int NOD_CHILDREN_OFF    = 0x6C;
// Player inner-struct offset to character NOD root (from GAM_PL_RenderSkeleton
// @ 0xefa305: v5 = *(NOD**)(this + 876)).
constexpr int PLAYER_CHAR_NOD     = 0x36C;

// ============================================================================
// Function types
// ============================================================================

typedef int (__thiscall *FillCamera_t)(void* thisPtr, void* cameraProp);
typedef void (__cdecl *SetCameraRotation_t)(void* cameraProp, float pitch, float roll, float yaw);
typedef float* (__thiscall *GetChestPosition_t)(void* playerEntity, float* outPos);
typedef int (__cdecl *GetBoneEndLoc_t)(void* skeleton, unsigned int boneId, float* outPos);
typedef int (__cdecl *GetEyeWorldPos_t)(void* skeleton, float* outPos);
typedef void (__cdecl *GetHandFireZone_t)(void* skeleton, float* outData);
typedef int (__cdecl *CalcFollowCameraPos_t)(void* playerEntity, float* rotMatrix, float* outCamPos, float* outLookAt, float distance);

// ============================================================================
// Global state (render-side only)
// ============================================================================

static uintptr_t g_gameBase = 0;
static FillCamera_t g_OriginalFillCamera = nullptr;
static SetCameraRotation_t g_SetCameraRotation = nullptr;
static GetChestPosition_t g_GetChestPosition = nullptr;
static GetBoneEndLoc_t g_GetBoneEndLoc = nullptr;
static GetEyeWorldPos_t g_GetEyeWorldPos = nullptr;
static GetHandFireZone_t g_GetHandFireZone = nullptr;
static CalcFollowCameraPos_t g_CalcFollowCamPos = nullptr;
static bool g_hookInstalled = false;

// Render-side state (not decision state — that's in CameraDirector)
static void* g_lastPlayerEntity = nullptr;

// 3PV Yaw smoothing
static float g_smoothedYaw = 0.0f;
static bool g_yawInitialized = false;

// 3PV distance smoothing
static float g_smoothedDistance = 2.5f;
static bool g_distanceInitialized = false;
static bool g_lookLockWasActive = false;
static int g_lastLookLockTarget = 0;

// KillCam render-side position tracking for slide interpolation
static float g_killCamKillerLastPos[3] = {};
static bool g_killCamKillerPosLocked = false;
static float g_killCamVictimLastPos[3] = {};
static bool g_killCamVictimPosLocked = false;
static bool g_killCamPhase2Entered = false;
static float g_killCamVantagePos[3] = {};
static float g_killCamVantageLookAt[3] = {};
static bool g_killCamVantageValid = false;
static bool g_killCamVantageAttempted = false;
static float g_killCamMoveStartPos[3] = {};
static bool g_killCamMoveStartValid = false;
static KillCamStyle g_killCamStyle = KillCamStyle::BulletTravel;

// Track last director state for detecting KillCam entry
static CameraState g_lastDirState = CameraState::Idle;

// Spectated player combat state
static SpectatedPlayerState g_spectatedState;
static int g_fpvLogCounter = 0;
static FILE* g_fpvLog = nullptr;

static void FpvLog(const char* fmt, ...) {
    if (!g_fpvLog) {
        g_fpvLog = fopen("autospectator_debug.log", "a");
        if (!g_fpvLog) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_fpvLog, fmt, args);
    fflush(g_fpvLog);
    va_end(args);
}

static void BeginLookLockLog(int target, const char* reason) {
    if (!g_lookLockWasActive || g_lastLookLockTarget != target) {
        FpvLog("[LookLock] start reason=%s target=%d orbit-yaw\n",
               reason ? reason : "unknown",
               target);
    }
    g_lookLockWasActive = true;
    g_lastLookLockTarget = target;
}

static void EndLookLockLog(const char* detail) {
    if (!g_lookLockWasActive) return;

    if (detail) {
        FpvLog("[LookLock] end target=%d %s\n", g_lastLookLockTarget, detail);
    } else {
        FpvLog("[LookLock] end target=%d\n", g_lastLookLockTarget);
    }

    g_lookLockWasActive = false;
    g_lastLookLockTarget = 0;
}

// ============================================================================
// Helper functions
// ============================================================================

static float NormalizeAngle(float angle) {
    while (angle > 3.14159265f) angle -= 6.28318530f;
    while (angle < -3.14159265f) angle += 6.28318530f;
    return angle;
}

static float LerpAngle(float from, float to, float t) {
    float diff = NormalizeAngle(to - from);
    return from + diff * t;
}

static float SmoothStep(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

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

// Find playerTable entry whose entity pointer matches the given entity.
// Returns the playerTable[i] entry (client struct with ntm offsets), or nullptr.
static void* FindClientStructByEntity(void* entity) {
    if (!entity) return nullptr;
    void** playerTable = (void**)(g_gameBase + 0x7AE9C8);
    for (int i = 0; i < 64; i++) {
        if (!playerTable[i]) continue;
        uintptr_t ent = *(uintptr_t*)((uintptr_t)playerTable[i] + 244);
        if (ent == (uintptr_t)entity) return playerTable[i];
    }
    return nullptr;
}

static void GetEntityPos(void* entity, float* out) {
    out[0] = *(float*)((uintptr_t)entity + 0xD0);
    out[1] = *(float*)((uintptr_t)entity + 0xD4);
    out[2] = *(float*)((uintptr_t)entity + 0xD8);
}

static bool GetBestKillCamPosition(int handle, float out[3]) {
    if (RealtimeHook_GetPredictedPlayerPosition(handle, out, 7000)) {
        return true;
    }

    void* entity = FindEntityByHandle(handle);
    if (!entity) return false;
    GetEntityPos(entity, out);
    return !(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);
}

static bool GetEntityBonePos(void* entity, unsigned int boneId, float out[3]) {
    if (!entity || !g_GetBoneEndLoc) return false;

    void** skeletonPtrPtr = *(void***)((char*)entity + PLAYER_SKELETON_PTR);
    if (!skeletonPtrPtr || !*skeletonPtrPtr) return false;

    return g_GetBoneEndLoc(*skeletonPtrPtr, boneId, out) != 0;
}

static bool GetPlayerAimPoint(int playerHandle, float out[3]) {
    void* playerEntity = FindEntityByHandle(playerHandle);
    if (GetEntityBonePos(playerEntity, BONE_NECK, out)) {
        return true;
    }

    if (GetBestKillCamPosition(playerHandle, out)) {
        out[2] += 1.25f;
        return true;
    }

    if (playerEntity) {
        GetEntityPos(playerEntity, out);
        out[2] += 1.25f;
        return true;
    }

    return false;
}

static bool Get3pvOrbitPivot(void* playerEntity, float out[3]) {
    if (!playerEntity) return false;

    if (GetEntityBonePos(playerEntity, BONE_NECK, out)) {
        return true;
    }

    if (GetEntityBonePos(playerEntity, BONE_CHEST, out)) {
        return true;
    }

    if (g_GetChestPosition && g_GetChestPosition(playerEntity, out)) {
        return true;
    }

    GetEntityPos(playerEntity, out);
    out[2] += 1.25f;
    return true;
}

static bool Get3pvOrbitYawToTarget(void* playerEntity, int targetHandle, float* outYaw) {
    float pivot[3];
    float target[3];
    if (!Get3pvOrbitPivot(playerEntity, pivot)
        || !GetPlayerAimPoint(targetHandle, target)) {
        return false;
    }

    float dx = target[0] - pivot[0];
    float dy = target[1] - pivot[1];
    if ((dx * dx + dy * dy) < 0.0001f) {
        return false;
    }

    *outYaw = atan2f(dx, dy) + YAW_OFFSET_3PV;
    return true;
}

static float ClampFloat(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static void CopyVec3(const float* src, float* dst) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

static float Vec3Distance(const float* a, const float* b) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void Vec3AtHeight(const float* base, float height, float out[3]) {
    out[0] = base[0];
    out[1] = base[1];
    out[2] = base[2] + height;
}

struct KillCamTargetView {
    float base[3];
    float aim[3];
    float samples[4][3];
    int sampleCount;
};

struct KillCamCandidateDir {
    float x;
    float y;
    float preference;
};

static void AddKillCamSample(KillCamTargetView& view, const float* pos) {
    if (view.sampleCount >= 4) return;

    for (int i = 0; i < view.sampleCount; ++i) {
        if (Vec3Distance(view.samples[i], pos) < 0.05f) {
            return;
        }
    }

    CopyVec3(pos, view.samples[view.sampleCount]);
    ++view.sampleCount;
}

static bool GetKillCamBasePosition(int handle, float out[3], bool* usedPredicted) {
    if (usedPredicted) *usedPredicted = false;

    if (RealtimeHook_GetPredictedPlayerPosition(handle, out, 7000)) {
        if (usedPredicted) *usedPredicted = true;
        return true;
    }

    void* entity = FindEntityByHandle(handle);
    if (!entity) return false;

    GetEntityPos(entity, out);
    return !(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);
}

static bool GetKillCamTargetView(int handle, KillCamTargetView& out) {
    memset(&out, 0, sizeof(out));

    bool usedPredicted = false;
    if (!GetKillCamBasePosition(handle, out.base, &usedPredicted)) {
        return false;
    }

    if (usedPredicted) {
        float neck[3], chest[3];
        Vec3AtHeight(out.base, 1.45f, neck);
        Vec3AtHeight(out.base, 1.05f, chest);
        Vec3AtHeight(out.base, 1.25f, out.aim);
        AddKillCamSample(out, neck);
        AddKillCamSample(out, chest);
        return true;
    }

    void* entity = FindEntityByHandle(handle);
    float neck[3], chest[3];
    bool haveNeck = GetEntityBonePos(entity, BONE_NECK, neck);
    bool haveChest = GetEntityBonePos(entity, BONE_CHEST, chest);

    if (haveNeck && haveChest) {
        out.aim[0] = (neck[0] + chest[0]) * 0.5f;
        out.aim[1] = (neck[1] + chest[1]) * 0.5f;
        out.aim[2] = (neck[2] + chest[2]) * 0.5f;
    } else if (haveNeck) {
        CopyVec3(neck, out.aim);
    } else if (haveChest) {
        CopyVec3(chest, out.aim);
        out.aim[2] += 0.20f;
    } else {
        Vec3AtHeight(out.base, 1.25f, out.aim);
    }

    if (haveNeck) {
        AddKillCamSample(out, neck);
    } else {
        Vec3AtHeight(out.base, 1.45f, neck);
        AddKillCamSample(out, neck);
    }

    if (haveChest) {
        AddKillCamSample(out, chest);
    } else {
        Vec3AtHeight(out.base, 1.05f, chest);
        AddKillCamSample(out, chest);
    }

    return true;
}

static bool KillCamTargetHasLos(const float* candidate, const KillCamTargetView& target) {
    if (!OctCollision_LineOfSight(candidate, target.aim)) {
        return false;
    }

    for (int i = 0; i < target.sampleCount; ++i) {
        if (!OctCollision_LineOfSight(candidate, target.samples[i])) {
            return false;
        }
    }

    return true;
}

static bool KillCamCandidateInsideMap(const float* candidate) {
    float boundsMin[3], boundsMax[3];
    if (!OctCollision_GetBounds(boundsMin, boundsMax)) {
        return false;
    }

    constexpr float BOUNDS_MARGIN = 0.05f;
    return candidate[0] >= boundsMin[0] + BOUNDS_MARGIN
        && candidate[1] >= boundsMin[1] + BOUNDS_MARGIN
        && candidate[2] >= boundsMin[2] + BOUNDS_MARGIN
        && candidate[0] <= boundsMax[0] - BOUNDS_MARGIN
        && candidate[1] <= boundsMax[1] - BOUNDS_MARGIN
        && candidate[2] <= boundsMax[2] - BOUNDS_MARGIN;
}

static bool ScoreKillCamCandidate(const float* candidate,
                                  const KillCamTargetView& killer,
                                  const KillCamTargetView& victim,
                                  const float* mid,
                                  float dirX,
                                  float dirY,
                                  float sidePreference,
                                  const float* currentCam,
                                  const CameraConfig& cfg,
                                  float* outScore) {
    if (!KillCamCandidateInsideMap(candidate)) {
        return false;
    }

    float minClearance = cfg.detachedKillCamMinClearance;
    if (minClearance < 0.0f) minClearance = 0.0f;

    float clearance = minClearance + 2.0f;
    if (PathGrid_IsReady()) {
        clearance = PathGrid_GetClearance(candidate[0], candidate[1], candidate[2]);
        if (clearance < minClearance) {
            return false;
        }
    }

    if (!KillCamTargetHasLos(candidate, killer)
        || !KillCamTargetHasLos(candidate, victim)) {
        return false;
    }

    float toMidX = candidate[0] - mid[0];
    float toMidY = candidate[1] - mid[1];
    float xyRadius = sqrtf(toMidX * toMidX + toMidY * toMidY);

    float absDot = 1.0f;
    if (xyRadius > 0.05f) {
        float viewX = toMidX / xyRadius;
        float viewY = toMidY / xyRadius;
        absDot = fabsf(viewX * dirX + viewY * dirY);
    }
    float sideScore = 1.0f - ClampFloat(absDot, 0.0f, 1.0f);

    float toKiller[3] = {
        killer.aim[0] - candidate[0],
        killer.aim[1] - candidate[1],
        killer.aim[2] - candidate[2]
    };
    float toVictim[3] = {
        victim.aim[0] - candidate[0],
        victim.aim[1] - candidate[1],
        victim.aim[2] - candidate[2]
    };
    float killerDist = sqrtf(toKiller[0] * toKiller[0] + toKiller[1] * toKiller[1] + toKiller[2] * toKiller[2]);
    float victimDist = sqrtf(toVictim[0] * toVictim[0] + toVictim[1] * toVictim[1] + toVictim[2] * toVictim[2]);

    float sepAngle = 0.0f;
    if (killerDist > 0.05f && victimDist > 0.05f) {
        float angleDot = (toKiller[0] * toVictim[0] + toKiller[1] * toVictim[1] + toKiller[2] * toVictim[2])
                       / (killerDist * victimDist);
        sepAngle = acosf(ClampFloat(angleDot, -1.0f, 1.0f));
    }

    float minRadius = cfg.detachedKillCamMinRadius;
    if (minRadius < 1.0f) minRadius = 1.0f;

    float minHeight = cfg.detachedKillCamMinHeight;
    float maxHeight = cfg.detachedKillCamMaxHeight;
    if (minHeight < 0.5f) minHeight = 0.5f;
    if (maxHeight < minHeight) maxHeight = minHeight;

    float heightRel = candidate[2] - mid[2];
    float desiredHeight = minHeight + (maxHeight - minHeight) * 0.45f;
    float minTargetDist = killerDist < victimDist ? killerDist : victimDist;

    float sepScore = 1.0f - fabsf(sepAngle - 0.32f) / 0.32f;
    sepScore = ClampFloat(sepScore, 0.0f, 1.0f);

    float score = 0.0f;
    score += sideScore * 34.0f;
    score += sidePreference * 12.0f;
    score += sepScore * 22.0f;
    score += clearance * (PathGrid_IsReady() ? 2.5f : 0.5f);
    score -= fabsf(killerDist - victimDist) * 0.35f;
    score -= fabsf(heightRel - desiredHeight) * 0.75f;

    if (absDot > 0.92f) score -= 9.0f;
    if (sepAngle < 0.08f) score -= 16.0f;
    if (sepAngle > 0.80f) score -= 10.0f;
    if (minTargetDist < 4.0f) score -= (4.0f - minTargetDist) * 9.0f;
    if (xyRadius < minRadius * 0.45f) score -= 16.0f;
    if (heightRel < minHeight + 0.35f) score -= 8.0f;

    float topDownRatio = heightRel / (xyRadius + 0.5f);
    if (topDownRatio > 0.90f) {
        score -= (topDownRatio - 0.90f) * 18.0f;
    }

    if (currentCam) {
        float jumpDist = Vec3Distance(currentCam, candidate);
        score -= jumpDist * 0.04f;
        if (OctCollision_LineOfSight(currentCam, candidate)) {
            score += 7.0f;
        } else {
            score -= 4.0f;
        }
    }

    *outScore = score;
    return true;
}

static void AddKillCamDirection(KillCamCandidateDir* dirs,
                                int* dirCount,
                                float x,
                                float y,
                                float preference) {
    if (*dirCount >= 10) return;

    float len = sqrtf(x * x + y * y);
    if (len < 0.01f) return;

    x /= len;
    y /= len;

    for (int i = 0; i < *dirCount; ++i) {
        float dot = dirs[i].x * x + dirs[i].y * y;
        if (dot > 0.985f) {
            if (preference > dirs[i].preference) {
                dirs[i].preference = preference;
            }
            return;
        }
    }

    dirs[*dirCount].x = x;
    dirs[*dirCount].y = y;
    dirs[*dirCount].preference = preference;
    ++(*dirCount);
}

static bool TryScoreKillCamCandidate(const float* candidate,
                                     const KillCamTargetView& killer,
                                     const KillCamTargetView& victim,
                                     const float* mid,
                                     float dirX,
                                     float dirY,
                                     float preference,
                                     const float* currentCam,
                                     const CameraConfig& cfg,
                                     float* bestScore,
                                     float* bestPos) {
    float score = 0.0f;
    if (!ScoreKillCamCandidate(candidate, killer, victim, mid,
                               dirX, dirY, preference, currentCam, cfg,
                               &score)) {
        return false;
    }

    if (score > *bestScore) {
        *bestScore = score;
        CopyVec3(candidate, bestPos);
    }
    return true;
}

static bool SolveKillCamVantage(int killerHandle,
                                int victimHandle,
                                const float* currentCam,
                                const CameraConfig& cfg,
                                float outPos[3],
                                float outLookAt[3]) {
    if (!OctCollision_IsLoaded()) {
        return false;
    }

    KillCamTargetView killer, victim;
    if (!GetKillCamTargetView(killerHandle, killer)
        || !GetKillCamTargetView(victimHandle, victim)) {
        return false;
    }

    float mid[3] = {
        (killer.base[0] + victim.base[0]) * 0.5f,
        (killer.base[1] + victim.base[1]) * 0.5f,
        (killer.base[2] + victim.base[2]) * 0.5f
    };

    outLookAt[0] = (killer.aim[0] + victim.aim[0]) * 0.5f;
    outLookAt[1] = (killer.aim[1] + victim.aim[1]) * 0.5f;
    outLookAt[2] = (killer.aim[2] + victim.aim[2]) * 0.5f;

    float dirX = victim.base[0] - killer.base[0];
    float dirY = victim.base[1] - killer.base[1];
    float separationXY = sqrtf(dirX * dirX + dirY * dirY);
    if (separationXY < 0.05f) {
        dirX = victim.aim[0] - killer.aim[0];
        dirY = victim.aim[1] - killer.aim[1];
        separationXY = sqrtf(dirX * dirX + dirY * dirY);
    }
    if (separationXY < 0.05f) {
        dirX = 0.0f;
        dirY = 1.0f;
    } else {
        dirX /= separationXY;
        dirY /= separationXY;
    }

    float sideX = -dirY;
    float sideY = dirX;

    float minRadius = cfg.detachedKillCamMinRadius;
    float maxRadius = cfg.detachedKillCamMaxRadius;
    if (minRadius < 1.0f) minRadius = 1.0f;
    if (maxRadius < 1.0f) maxRadius = 1.0f;
    if (maxRadius < minRadius) maxRadius = minRadius;

    float separation = Vec3Distance(killer.base, victim.base);
    float framingRadius = ClampFloat(separation * 0.85f + 2.0f, minRadius, maxRadius);
    float midRadius = (minRadius + maxRadius) * 0.5f;
    float radii[4] = { framingRadius, midRadius, minRadius, maxRadius };

    float minHeight = cfg.detachedKillCamMinHeight;
    float maxHeight = cfg.detachedKillCamMaxHeight;
    if (minHeight < 0.5f) minHeight = 0.5f;
    if (maxHeight < 0.5f) maxHeight = 0.5f;
    if (maxHeight < minHeight) maxHeight = minHeight;

    float midHeight = (minHeight + maxHeight) * 0.5f;
    float heights[3] = { midHeight, minHeight, maxHeight };

    KillCamCandidateDir dirs[10];
    int dirCount = 0;
    AddKillCamDirection(dirs, &dirCount,  sideX,          sideY,          1.00f);
    AddKillCamDirection(dirs, &dirCount, -sideX,         -sideY,          1.00f);
    AddKillCamDirection(dirs, &dirCount,  sideX + dirX,   sideY + dirY,   0.78f);
    AddKillCamDirection(dirs, &dirCount, -sideX + dirX,  -sideY + dirY,   0.78f);
    AddKillCamDirection(dirs, &dirCount,  sideX - dirX,   sideY - dirY,   0.72f);
    AddKillCamDirection(dirs, &dirCount, -sideX - dirX,  -sideY - dirY,   0.72f);
    AddKillCamDirection(dirs, &dirCount, -dirX,          -dirY,           0.35f);
    AddKillCamDirection(dirs, &dirCount,  dirX,           dirY,           0.28f);

    float bestScore = -1.0e30f;
    float bestPos[3] = {};

    for (int d = 0; d < dirCount; ++d) {
        for (int r = 0; r < 4; ++r) {
            for (int h = 0; h < 3; ++h) {
                float candidate[3] = {
                    mid[0] + dirs[d].x * radii[r],
                    mid[1] + dirs[d].y * radii[r],
                    mid[2] + heights[h]
                };

                TryScoreKillCamCandidate(candidate, killer, victim, mid,
                                         dirX, dirY, dirs[d].preference,
                                         currentCam, cfg, &bestScore, bestPos);
            }
        }
    }

    float topOffsets[5][2] = {
        { 0.0f, 0.0f },
        {  sideX * minRadius * 0.35f,  sideY * minRadius * 0.35f },
        { -sideX * minRadius * 0.35f, -sideY * minRadius * 0.35f },
        {  dirX  * minRadius * 0.35f,  dirY  * minRadius * 0.35f },
        { -dirX  * minRadius * 0.35f, -dirY  * minRadius * 0.35f }
    };

    for (int i = 0; i < 5; ++i) {
        float candidate[3] = {
            mid[0] + topOffsets[i][0],
            mid[1] + topOffsets[i][1],
            mid[2] + maxHeight
        };

        TryScoreKillCamCandidate(candidate, killer, victim, mid,
                                 dirX, dirY, 0.12f, currentCam, cfg,
                                 &bestScore, bestPos);
    }

    if (bestScore <= -1.0e29f) {
        return false;
    }

    CopyVec3(bestPos, outPos);
    std::cout << "[Camera] KillCam solved vantage pos=("
              << outPos[0] << ", " << outPos[1] << ", " << outPos[2]
              << ") score=" << bestScore << "\n";
    return true;
}

static bool EnsureKillCamVantage(int killerHandle,
                                 int victimHandle,
                                 const float* currentCam,
                                 const CameraConfig& cfg) {
    if (g_killCamVantageValid) {
        return true;
    }
    if (g_killCamVantageAttempted) {
        return false;
    }

    g_killCamVantageAttempted = true;
    g_killCamVantageValid = SolveKillCamVantage(killerHandle,
                                                victimHandle,
                                                currentCam,
                                                cfg,
                                                g_killCamVantagePos,
                                                g_killCamVantageLookAt);
    if (!g_killCamVantageValid) {
        std::cout << "[Camera] KillCam vantage unavailable; using safe spectator fallback\n";
    }
    return g_killCamVantageValid;
}

static bool IsCloseKillCamLookLockActive(const CameraConfig& cfg) {
    float killTime = cfg.detachedKillCamFollowDuration
                   + cfg.detachedKillCamRepositionDuration;
    if (killTime < 0.1f) killTime = 0.1f;
    float releaseTime = killTime + cfg.detachedKillCamHoldDuration;
    if (releaseTime < killTime + 0.1f) releaseTime = killTime + 0.1f;

    float advance = cfg.killLookLockAdvance;
    if (advance < 0.1f) advance = 0.1f;
    if (advance > killTime) advance = killTime;

    float elapsed = CameraDirector_GetKillCamElapsed();
    return elapsed >= (killTime - advance) && elapsed < releaseTime;
}

static bool Get3pvLookLockTarget(CameraState dirState,
                                 KillCamStyle kcStyle,
                                 const CameraConfig& cfg,
                                 int* outTarget,
                                 const char** outReason) {
    if (WorldCameraTracker_GetCurrentCamType() != 2) {
        return false;
    }

    if (dirState == CameraState::FlagWatch
        || dirState == CameraState::FollowPlayer) {
        int killer = 0;
        if (CameraDirector_GetFlagCarrierKillLook(&killer, nullptr, nullptr, nullptr)) {
            if (outTarget) *outTarget = killer;
            if (outReason) {
                *outReason = dirState == CameraState::FlagWatch
                    ? "flag-victim"
                    : "victim";
            }
            return killer != 0;
        }
    }

    if (dirState == CameraState::KillCam
        && kcStyle == KillCamStyle::DetachedVantage
        && IsCloseKillCamLookLockActive(cfg)) {
        int victim = CameraDirector_GetKillCamVictimHandle();
        if (outTarget) *outTarget = victim;
        if (outReason) *outReason = "killcam";
        return victim != 0;
    }

    return false;
}

static void AimCameraAt(void* cameraProp, const float* camPos, const float* lookAt) {
    float dx = lookAt[0] - camPos[0];
    float dy = lookAt[1] - camPos[1];
    float dz = lookAt[2] - camPos[2];
    float horizDist = sqrtf(dx * dx + dy * dy);

    float yaw = (horizDist > 0.01f) ? atan2f(-dx, dy) : 0.0f;
    float pitch = (horizDist > 0.01f) ? -atan2f(dz, horizDist) : 0.0f;

    if (g_SetCameraRotation) {
        g_SetCameraRotation(cameraProp, pitch, 0.0f, yaw);
    }
}

static bool GetBoneRotation(void* skeleton, unsigned int boneId, float* outYaw, float* outPitch) {
    unsigned int boneCount = *(unsigned int*)((char*)skeleton + 0x18);
    if (boneId >= boneCount) return false;

    void** boneArray = *(void***)((char*)skeleton + 0x1C);
    if (!boneArray) return false;

    void* bone = boneArray[boneId];
    if (!bone) return false;

    void* worldTransform = *(void**)((char*)bone + 0x88);
    float* matrix;
    if (worldTransform) {
        matrix = (float*)((char*)worldTransform + 0x1C);
    } else {
        matrix = (float*)((char*)bone + 0x1C);
    }

    float fwdX = matrix[4];
    float fwdY = matrix[5];
    float fwdZ = matrix[6];

    *outYaw = atan2f(fwdX, fwdY);
    *outPitch = asinf(fwdZ);

    return true;
}

// ============================================================================
// Hook function
// ============================================================================

// ============================================================================
// Hide spectated player's head NODs while in FPV (so face/skull doesn't clip
// into the eye camera). BES character submeshes follow naming
// {LOD}{DAMAGE}_{BODYPART}, so head meshes are 0A_head..2C_head.
// ============================================================================

struct HiddenNod { void* nod; uint32_t origFlags; };
static std::vector<HiddenNod> g_hiddenHeadNods;
static void* g_lastHeadHideEntity = nullptr;

// SEH-wrapped raw memory ops — kept in their own functions because /EHsc + std::vector
// in caller would trigger C2712 if __try were inline.
static bool SafeReadPtr(void* addr, void** out) {
    __try { *out = *(void**)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeReadDword(void* addr, uint32_t* out) {
    __try { *out = *(uint32_t*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeWriteDword(void* addr, uint32_t val) {
    __try { *(uint32_t*)addr = val; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// First-run diagnostic dump of traversed NOD names, so we can spot the real naming.
static bool g_headDumpOnce = false;
static int  g_headDumpLines = 0;

static void NodFindByName(void* nod, const char* name, std::vector<void*>& out, int depth) {
    if (!nod || depth > 32) return;
    char nameBuf[64];
    uint32_t nkidsRaw = 0;
    void* kidsPtr = nullptr;
    // Read name in 4-byte chunks
    for (int off = 0; off < 64; off += 4) {
        if (!SafeReadDword((char*)nod + NOD_NAME_OFF + off, (uint32_t*)(nameBuf + off))) {
            nameBuf[off] = 0; break;
        }
    }
    nameBuf[63] = 0;

    if (g_headDumpOnce && g_headDumpLines < 200 && nameBuf[0]) {
        FpvLog("[NodDump] d=%d nod=0x%08X name='%s'\n",
            depth, (unsigned)(uintptr_t)nod, nameBuf);
        g_headDumpLines++;
    }

    if (strncmp(nameBuf, name, 63) == 0) out.push_back(nod);

    if (!SafeReadDword((char*)nod + NOD_CHILD_COUNT_OFF, &nkidsRaw)) return;
    if (!SafeReadPtr((char*)nod + NOD_CHILDREN_OFF, &kidsPtr)) return;
    int nkids = (int)nkidsRaw;
    if (!kidsPtr || nkids <= 0 || nkids > 4096) return;
    for (int i = 0; i < nkids; ++i) {
        void* child = nullptr;
        if (SafeReadPtr((char*)kidsPtr + 4 * i, &child)) {
            NodFindByName(child, name, out, depth + 1);
        }
    }
}

static void RestoreHiddenHead() {
    for (auto& h : g_hiddenHeadNods) {
        if (h.nod) SafeWriteDword((char*)h.nod + NOD_FLAGS_OFF, h.origFlags);
    }
    g_hiddenHeadNods.clear();
    g_lastHeadHideEntity = nullptr;
}

static void HideHeadFor(void* playerEntity) {
    if (!playerEntity) { RestoreHiddenHead(); return; }
    if (playerEntity == g_lastHeadHideEntity) return;

    RestoreHiddenHead();
    void* charNod = nullptr;
    if (!SafeReadPtr((char*)playerEntity + PLAYER_CHAR_NOD, &charNod) || !charNod) return;

    static const char* kHeadNames[] = {
        "0A_head", "0B_head", "0C_head",
        "1A_head", "1B_head", "1C_head",
        "2A_head", "2B_head", "2C_head",
    };
    std::vector<void*> hits;
    // First call also dumps every name in the tree to the log so we can see the real layout.
    g_headDumpOnce = (g_headDumpLines < 200);
    g_headDumpLines = 0;
    for (const char* nm : kHeadNames) NodFindByName(charNod, nm, hits, 0);
    g_headDumpOnce = false;

    for (void* nod : hits) {
        uint32_t orig = 0;
        if (SafeReadDword((char*)nod + NOD_FLAGS_OFF, &orig)) {
            g_hiddenHeadNods.push_back({nod, orig});
            SafeWriteDword((char*)nod + NOD_FLAGS_OFF, orig | 1u);
        }
    }
    g_lastHeadHideEntity = playerEntity;
    FpvLog("[HeadHide] entity=0x%08X charNod=0x%08X hidden=%d\n",
        (unsigned)(uintptr_t)playerEntity, (unsigned)(uintptr_t)charNod, (int)g_hiddenHeadNods.size());
}

int __fastcall Hooked_FillCamera(void* thisPtr, void* edx_unused, void* cameraProp) {
    DelayManager::ReloadConfigIfNeeded();

    int mode = *(int*)((char*)thisPtr + SPECTATOR_CTRL_MODE);

    if (mode != SPECTATOR_MODE_FREE) {
        RestoreHiddenHead();
        return g_OriginalFillCamera(thisPtr, cameraProp);
    }

    CameraState dirState = CameraDirector_GetState();
    KillCamPhase kcPhase = CameraDirector_GetKillCamPhase();
    KillCamStyle kcStyle = CameraDirector_GetKillCamStyle();
    const CameraConfig& cfg = CameraDirector_GetConfig();

    // Detect player change and reset render state
    void* client = *(void**)((char*)thisPtr + SPECTATOR_CTRL_CLIENT);
    void* playerEntity = nullptr;
    if (client) {
        playerEntity = *(void**)((char*)client + CLIENT_PLAYER_ENTITY);
    }

    if (playerEntity && playerEntity != g_lastPlayerEntity) {
        g_yawInitialized = false;
        g_distanceInitialized = false;
        g_lastPlayerEntity = playerEntity;
        g_fpvLogCounter = 0; // force immediate log on target switch

        // Log addresses for CE inspection
        void* ptEntry = FindClientStructByEntity(playerEntity);
        FpvLog("[FPV-ADDR] thisPtr=0x%08X client=0x%08X entity=0x%08X ptEntry=0x%08X\n",
            (uintptr_t)thisPtr, (uintptr_t)client, (uintptr_t)playerEntity, (uintptr_t)ptEntry);
    }

    // Read spectated player combat state
    if (playerEntity) {
        // Find the playerTable entry for this entity (has ntm offsets for stance/anim/fire)
        void* playerTableEntry = FindClientStructByEntity(playerEntity);
        g_spectatedState = ReadPlayerCombatState(playerTableEntry, playerEntity, g_gameBase);
        if (g_spectatedState.valid) {
            g_spectatedState.playerHandle = playerTableEntry ? *(int*)((char*)playerTableEntry + 0x00) : 0;

            // Log every ~120 frames (~2 seconds at 60fps)
            if (++g_fpvLogCounter >= 120) {
                g_fpvLogCounter = 0;
                const char* stanceName = "STAND";
                if (g_spectatedState.stance == STANCE_CROUCH) stanceName = "CROUCH";
                else if (g_spectatedState.stance == STANCE_PRONE) stanceName = "PRONE";

                FpvLog("[FPV] weapon=%s(%d) slot=%d stance=%s anim=%d stanceFlag=0x%X\n",
                    g_spectatedState.weaponName, g_spectatedState.weaponId,
                    g_spectatedState.activeSlotIndex, stanceName,
                    g_spectatedState.animId, g_spectatedState.weaponState);
            }
        }
    }

    // Detect KillCam start (transition from non-KillCam to KillCam)
    if (dirState == CameraState::KillCam && g_lastDirState != CameraState::KillCam) {
        // New KillCam started — snapshot positions
        int killerHandle = CameraDirector_GetKillCamKillerHandle();
        int victimHandle = CameraDirector_GetKillCamVictimHandle();
        g_killCamStyle = kcStyle;

        if (!GetBestKillCamPosition(killerHandle, g_killCamKillerLastPos)) {
            memset(g_killCamKillerLastPos, 0, sizeof(g_killCamKillerLastPos));
        }

        if (!GetBestKillCamPosition(victimHandle, g_killCamVictimLastPos)) {
            memset(g_killCamVictimLastPos, 0, sizeof(g_killCamVictimLastPos));
        }

        g_killCamKillerPosLocked = false;
        g_killCamVictimPosLocked = false;
        g_killCamPhase2Entered = false;
        g_killCamVantageValid = false;
        g_killCamVantageAttempted = false;
        g_killCamMoveStartValid = false;

        if (g_killCamStyle == KillCamStyle::DetachedVantage) {
            g_yawInitialized = false;
            std::cout << "[Camera] Close KillCam: solving collision-safe vantage\n";
        }
    }

    // 3PV pre-processing: run unless the bullet-travel KillCam takes full camera control.
    bool killCamOverridesCamera = dirState == CameraState::KillCam
        && kcPhase == KillCamPhase::Transition
        && g_killCamStyle == KillCamStyle::BulletTravel;
    bool do3pvPreprocess = !killCamOverridesCamera;
    int lookLockTarget = 0;
    const char* lookLockReason = nullptr;
    bool lookLockActive = Get3pvLookLockTarget(dirState, kcStyle, cfg,
                                               &lookLockTarget,
                                               &lookLockReason);
    bool lookLockApplied = false;

    if (do3pvPreprocess && playerEntity) {
        void** skeletonPtrPtr = *(void***)((char*)playerEntity + PLAYER_SKELETON_PTR);
        float yaw = 0.0f;
        float pitch = 0.0f;

        if (skeletonPtrPtr && *skeletonPtrPtr) {
            void* skeleton = *skeletonPtrPtr;

            if (GetBoneRotation(skeleton, BONE_HEAD, &yaw, &pitch)) {
                float desiredYaw = yaw + YAW_OFFSET_3PV;
                float lookLockYaw = 0.0f;
                if (lookLockActive
                    && Get3pvOrbitYawToTarget(playerEntity, lookLockTarget, &lookLockYaw)) {
                    desiredYaw = lookLockYaw;
                    lookLockApplied = true;
                }

                if (pitch > MAX_PITCH_3PV) pitch = MAX_PITCH_3PV;
                if (pitch < -MAX_PITCH_3PV) pitch = -MAX_PITCH_3PV;

                if (!g_yawInitialized) {
                    g_smoothedYaw = desiredYaw;
                    g_yawInitialized = true;
                }

                g_smoothedYaw = LerpAngle(g_smoothedYaw, desiredYaw, cfg.tpvYawSmoothFactor);

                int cameraIndex = *(int*)((char*)thisPtr + 0x28);
                char* cameraEntry = (char*)thisPtr + 44 + 20 * cameraIndex;
                *(float*)(cameraEntry + 8) = pitch;
                *(float*)(cameraEntry + 12) = g_smoothedYaw;
            }
        }
    }

    // Call original function
    int result = g_OriginalFillCamera(thisPtr, cameraProp);

    // Drone camera: override position/rotation entirely, skip all post-processing
    if (dirState == CameraState::Drone && result) {
        EndLookLockLog(nullptr);
        RestoreHiddenHead();
        float pos[3], pitch, yaw;
        DroneCamera_GetCameraState(pos, &pitch, &yaw);
        float* camPos = (float*)cameraProp;
        camPos[0] = pos[0]; camPos[1] = pos[1]; camPos[2] = pos[2];
        if (g_SetCameraRotation) {
            g_SetCameraRotation(cameraProp, pitch, 0.0f, yaw);
        }
        g_lastDirState = dirState;
        return result;
    }

    int directorCamType = WorldCameraTracker_GetCurrentCamType();
    bool useFpvCamera = result && playerEntity
        && dirState != CameraState::KillCam
        && dirState != CameraState::Drone
        && directorCamType == 2
        && CameraDirector_ShouldUseFpv();

    if (!useFpvCamera) {
        RestoreHiddenHead();
    }

    // FPV camera: position at player's eye, rotate with head bone
    if (useFpvCamera) {
        HideHeadFor(playerEntity);
        void** skeletonPtrPtr = *(void***)((char*)playerEntity + PLAYER_SKELETON_PTR);
        if (skeletonPtrPtr && *skeletonPtrPtr) {
            void* skeleton = *skeletonPtrPtr;
            float eyePos[3];

            // Anchor camera at the Neck bone rather than the eye so the head
            // mesh doesn't clip into view. Falls back to eye position if the
            // neck bone lookup fails.
            bool havePos = false;
            if (g_GetBoneEndLoc && g_GetBoneEndLoc(skeleton, BONE_NECK, eyePos)) {
                havePos = true;
            } else if (g_GetEyeWorldPos && g_GetEyeWorldPos(skeleton, eyePos)) {
                havePos = true;
            }
            if (havePos) {
                float* camPos = (float*)cameraProp;

                // Yaw: player entity applied yaw at +0xFC (dword 63) — same field
                // GAM_PL_GetRotation returns; the authoritative game yaw.
                // Pitch: entity applied field (+0xF8) with a 10° down tilt.
                constexpr float FPV_PITCH_OFFSET = -10.0f * 3.14159265f / 180.0f;
                float entYaw = *(float*)((char*)playerEntity + 0xFC);
                float yaw    = -entYaw;
                float pitch  = *(float*)((char*)playerEntity + 0xF8) + FPV_PITCH_OFFSET;

                // Offset camera 10cm back (opposite to player facing) and 10cm down.
                // Back = -forward. In VC's coord convention forward ~ (-sin(yaw), cos(yaw), 0).
                const float BACK_OFFSET = 0.10f;
                const float DOWN_OFFSET = 0.10f;
                float fx = -sinf(entYaw);
                float fy =  cosf(entYaw);
                camPos[0] = eyePos[0] - fx * BACK_OFFSET;
                camPos[1] = eyePos[1] - fy * BACK_OFFSET;
                camPos[2] = eyePos[2] - DOWN_OFFSET;

                if (g_SetCameraRotation) {
                    g_SetCameraRotation(cameraProp, pitch, 0.0f, yaw);
                }
                {

                    // Position FPV hands skeleton using entity data directly
                    uintptr_t fpvSkel = (uintptr_t)FpvViewmodel_GetSkeleton();
                    if (fpvSkel) {
                        // Position from entity torso (+0xD0, +0xD4, +0xD8)
                        float* entF = (float*)playerEntity;
                        float ex = *(float*)((char*)playerEntity + 0xD0);
                        float ey = *(float*)((char*)playerEntity + 0xD4);
                        float ez = *(float*)((char*)playerEntity + 0xD8);

                        // Rotation from entity fields
                        // player[61] = pitch (byte offset 0xF4)
                        // player[63] = yaw (byte offset 0xFC)
                        float entPitch = *((float*)playerEntity + 61);
                        float entYaw   = *((float*)playerEntity + 63);

                        *(float*)(fpvSkel + 16) = ex;
                        *(float*)(fpvSkel + 20) = ey;
                        *(float*)(fpvSkel + 24) = ez;
                        *(float*)(fpvSkel + 28) = entYaw;
                        *(float*)(fpvSkel + 32) = 0.0f;
                        *(float*)(fpvSkel + 36) = entPitch;
                    }
                }

                // Signal FPV viewmodel to render this frame (actual render happens in GAM_DoTick hook)
                FpvViewmodel_RenderFrame(playerEntity);
            }
        }

        g_lastDirState = dirState;
        EndLookLockLog(nullptr);
        return result;
    }

    if (lookLockApplied && result) {
        BeginLookLockLog(lookLockTarget, lookLockReason);
    } else if (g_lookLockWasActive) {
        EndLookLockLog(lookLockActive ? "target-lost" : nullptr);
    }

    // 3PV Post-processing: distance smoothing (skip during KillCam Transition)
    if (do3pvPreprocess && result && playerEntity && g_GetChestPosition) {
        float playerPos[3];
        g_GetChestPosition(playerEntity, playerPos);

        float* camPos = (float*)cameraProp;

        float dx = camPos[0] - playerPos[0];
        float dy = camPos[1] - playerPos[1];
        float dz = camPos[2] - playerPos[2];
        float originalDistance = sqrtf(dx * dx + dy * dy + dz * dz);

        float targetDistance = originalDistance;
        if (targetDistance > cfg.tpvMaxDistance)
            targetDistance = cfg.tpvMaxDistance;

        if (!g_distanceInitialized) {
            g_smoothedDistance = targetDistance;
            g_distanceInitialized = true;
        } else {
            float factor = (targetDistance < g_smoothedDistance)
                ? cfg.tpvZoomInFactor
                : cfg.tpvZoomOutFactor;
            g_smoothedDistance += (targetDistance - g_smoothedDistance) * factor;
        }

        if (g_smoothedDistance > cfg.tpvMaxDistance)
            g_smoothedDistance = cfg.tpvMaxDistance;

        if (originalDistance > 0.01f) {
            float scale = g_smoothedDistance / originalDistance;
            camPos[0] = playerPos[0] + dx * scale;
            camPos[1] = playerPos[1] + dy * scale;
            camPos[2] = playerPos[2] + dz * scale + 0.5f;
        }
    }

    // World camera auto-zoom: reduce FOV when player is far from camera
    {
        static float s_smoothedFovScale = 1.0f;
        static int s_prevCamType = -1;

        int wcType = WorldCameraTracker_GetCurrentCamType();
        if (wcType == 0 || wcType == 1) {
            float wcPos[3];
            if (WorldCameraTracker_GetCurrentCamPos(wcPos) && playerEntity) {
                float playerPos3[3];
                GetEntityPos(playerEntity, playerPos3);

                float dx = wcPos[0] - playerPos3[0];
                float dy = wcPos[1] - playerPos3[1];
                float dz = wcPos[2] - playerPos3[2];
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);

                float targetFovScale = 1.0f;
                if (dist > cfg.worldCamZoomStartDist) {
                    float t = (dist - cfg.worldCamZoomStartDist)
                            / (cfg.worldCamZoomMaxDist - cfg.worldCamZoomStartDist);
                    if (t > 1.0f) t = 1.0f;
                    targetFovScale = 1.0f - t * (1.0f - cfg.worldCamZoomMaxFactor);
                }

                // Reset smoothing on camera type transition
                if (s_prevCamType != 0 && s_prevCamType != 1) {
                    s_smoothedFovScale = targetFovScale;
                } else {
                    s_smoothedFovScale += (targetFovScale - s_smoothedFovScale) * cfg.worldCamZoomSmoothFactor;
                }

                float* fovScale = (float*)((char*)cameraProp + 0x0C);
                *fovScale *= s_smoothedFovScale;
            }
        } else {
            s_smoothedFovScale = 1.0f;
        }
        s_prevCamType = wcType;
    }

    // KillCam phased logic
    if (dirState == CameraState::KillCam && result) {
        float elapsed = CameraDirector_GetKillCamElapsed();
        int killerHandle = CameraDirector_GetKillCamKillerHandle();
        int victimHandle = CameraDirector_GetKillCamVictimHandle();
        float* liveCamPos = (float*)cameraProp;
        bool hasSolvedVantage = EnsureKillCamVantage(killerHandle,
                                                     victimHandle,
                                                     liveCamPos,
                                                     cfg);

        if (g_killCamStyle == KillCamStyle::DetachedVantage) {
            if (hasSolvedVantage) {
                float waitEnd = cfg.detachedKillCamFollowDuration;
                float transEnd = waitEnd + cfg.detachedKillCamRepositionDuration;

                if (kcPhase == KillCamPhase::Transition) {
                    if (!g_killCamMoveStartValid) {
                        CopyVec3(liveCamPos, g_killCamMoveStartPos);
                        g_killCamMoveStartValid = true;
                        std::cout << "[Camera] KillCam moving to solved vantage\n";
                    }

                    float denom = transEnd - waitEnd;
                    float t = denom > 0.001f ? SmoothStep((elapsed - waitEnd) / denom) : 1.0f;
                    liveCamPos[0] = g_killCamMoveStartPos[0] + (g_killCamVantagePos[0] - g_killCamMoveStartPos[0]) * t;
                    liveCamPos[1] = g_killCamMoveStartPos[1] + (g_killCamVantagePos[1] - g_killCamMoveStartPos[1]) * t;
                    liveCamPos[2] = g_killCamMoveStartPos[2] + (g_killCamVantagePos[2] - g_killCamMoveStartPos[2]) * t;
                    AimCameraAt(cameraProp, liveCamPos, g_killCamVantageLookAt);
                }

                if (kcPhase == KillCamPhase::Attached) {
                    if (!g_killCamPhase2Entered) {
                        g_killCamPhase2Entered = true;
                        std::cout << "[Camera] KillCam fixed solved vantage hold\n";
                    }

                    CopyVec3(g_killCamVantagePos, liveCamPos);
                    AimCameraAt(cameraProp, liveCamPos, g_killCamVantageLookAt);
                }
            }
        } else {
            // Look up entities, snapshot positions
            void* killerEntity = FindEntityByHandle(killerHandle);
            void* victimEntity = FindEntityByHandle(victimHandle);

            float killerPos[3], victimPos[3];

            if (killerEntity) {
                GetEntityPos(killerEntity, killerPos);
                memcpy(g_killCamKillerLastPos, killerPos, sizeof(killerPos));
            } else {
                if (!g_killCamKillerPosLocked) g_killCamKillerPosLocked = true;
                memcpy(killerPos, g_killCamKillerLastPos, sizeof(killerPos));
            }

            if (victimEntity) {
                GetEntityPos(victimEntity, victimPos);
                memcpy(g_killCamVictimLastPos, victimPos, sizeof(victimPos));
            } else {
                if (!g_killCamVictimPosLocked) g_killCamVictimPosLocked = true;
                memcpy(victimPos, g_killCamVictimLastPos, sizeof(victimPos));
            }

            float waitEnd = cfg.killCamWaitDuration;
            float transEnd = waitEnd + cfg.killCamTransitionDuration;

            // Phase 1 (transition): slide camera from killer to victim
            if (kcPhase == KillCamPhase::Transition) {
                float denom = transEnd - waitEnd;
                float t = denom > 0.001f ? SmoothStep((elapsed - waitEnd) / denom) : 1.0f;
                float* camPos = (float*)cameraProp;

                camPos[0] = killerPos[0] + (victimPos[0] - killerPos[0]) * t;
                camPos[1] = killerPos[1] + (victimPos[1] - killerPos[1]) * t;
                camPos[2] = killerPos[2] + (victimPos[2] - killerPos[2]) * t + cfg.killCamSlideHeight;

                float lookAt[3] = { victimPos[0], victimPos[1], victimPos[2] + 1.0f };
                AimCameraAt(cameraProp, camPos, lookAt);
            }

            // Phase 2 (attached): hold a fixed post-kill shot for a few seconds
            // only when the collision solver found a clear dual-LOS shot.
            if (kcPhase == KillCamPhase::Attached) {
                if (hasSolvedVantage) {
                    if (!g_killCamPhase2Entered) {
                        g_killCamPhase2Entered = true;
                        std::cout << "[Camera] KillCam phase 2: solved post-kill hold\n";
                    }

                    CopyVec3(g_killCamVantagePos, liveCamPos);
                    AimCameraAt(cameraProp, liveCamPos, g_killCamVantageLookAt);
                } else if (!g_killCamPhase2Entered) {
                    g_killCamPhase2Entered = true;
                    std::cout << "[Camera] KillCam phase 2: no solved vantage, keeping spectator fallback\n";
                }
            }
        }
    }

    g_lastDirState = dirState;

    return result;
}

// ============================================================================
// Public functions
// ============================================================================

bool InitFirstPersonCamera(uintptr_t gameBase) {
    g_gameBase = gameBase;

    std::cout << "[FirstPerson] Initializing camera hook...\n";
    std::cout << "[FirstPerson] game.dll base: 0x" << std::hex << gameBase << std::dec << "\n";

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        std::cout << "[FirstPerson] Failed to initialize MinHook: " << MH_StatusToString(status) << "\n";
        return false;
    }

    uintptr_t fillCameraAddr = gameBase + OFFSET_FILL_CAMERA;
    g_GetChestPosition = (GetChestPosition_t)(gameBase + OFFSET_GET_CHEST_POS);
    g_CalcFollowCamPos = (CalcFollowCameraPos_t)(gameBase + OFFSET_CALC_FOLLOW_CAM);

    g_SetCameraRotation = *(SetCameraRotation_t*)(gameBase + OFFSET_SET_CAM_ROTATION_IMPORT);
    g_GetBoneEndLoc = *(GetBoneEndLoc_t*)(gameBase + OFFSET_GET_BONE_LOC_IMPORT);
    g_GetEyeWorldPos = *(GetEyeWorldPos_t*)(gameBase + OFFSET_GET_EYE_POS_IMPORT);
    g_GetHandFireZone = *(GetHandFireZone_t*)(gameBase + OFFSET_GET_HAND_FIRE_IMPORT);

    std::cout << "[FirstPerson] FillCamera addr: 0x" << std::hex << fillCameraAddr << "\n";
    std::cout << "[FirstPerson] SetCamRotation addr: 0x" << (uintptr_t)g_SetCameraRotation << std::dec << "\n";

    status = MH_CreateHook(
        (LPVOID)fillCameraAddr,
        (LPVOID)&Hooked_FillCamera,
        (LPVOID*)&g_OriginalFillCamera
    );

    if (status != MH_OK) {
        std::cout << "[FirstPerson] Failed to create hook: " << MH_StatusToString(status) << "\n";
        return false;
    }

    status = MH_EnableHook((LPVOID)fillCameraAddr);
    if (status != MH_OK) {
        std::cout << "[FirstPerson] Failed to enable hook: " << MH_StatusToString(status) << "\n";
        MH_RemoveHook((LPVOID)fillCameraAddr);
        return false;
    }

    g_hookInstalled = true;
    std::cout << "[FirstPerson] Camera hook installed successfully!\n";

    return true;
}

void ShutdownFirstPersonCamera() {
    RestoreHiddenHead();
    if (g_hookInstalled) {
        std::cout << "[FirstPerson] Shutting down...\n";
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_hookInstalled = false;
    }
    if (g_fpvLog) {
        fclose(g_fpvLog);
        g_fpvLog = nullptr;
    }
}

const SpectatedPlayerState& GetSpectatedPlayerState() {
    return g_spectatedState;
}
