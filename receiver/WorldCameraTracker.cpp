#define _CRT_SECURE_NO_WARNINGS
#include "WorldCameraTracker.h"
#include <cstdio>
#include <cmath>
#include <atomic>
#include "OctCollision.h"

typedef void (__thiscall *SpectCamUpdateFunc)(int* thisPtr);

// CAM_Spline_Evaluate(splinePtr, time, outFloat10, outExtra)
// Returns BOOL. outFloat10 receives [x,y,z, ...] at index 0-2.
typedef int (__cdecl *CAM_Spline_EvaluateFunc)(int splinePtr, float time, float* out, void* extra);
static CAM_Spline_EvaluateFunc g_CAM_Spline_Evaluate = nullptr;

static uintptr_t g_baseGame = 0;
static std::atomic<int> g_targetHandle{ 0 };
static int g_currentCamIndex = -1;
static int g_currentCamType = 0; // 0=static, 1=dynamic, 2=player camera
static int g_frameCounter = 0;
static int g_holdFrames = 0; // how long current camera has been active

constexpr float MAX_CAM_DIST = 18.0f;
constexpr float MAX_CAM_DIST_SQ = MAX_CAM_DIST * MAX_CAM_DIST;
constexpr int SWITCH_COOLDOWN_FRAMES = 240; // ~4 seconds at 60fps
constexpr int MAX_HOLD_FRAMES = 600; // ~10 seconds at 60fps
static int g_switchCooldownFrames = 0;
static bool g_initialized = false;

constexpr uintptr_t SPECT_CAM_UPDATE_OFFSET = 0x148100;
constexpr uintptr_t PLAYER_TABLE_OFFSET = 0x7AE9C8;
constexpr uintptr_t CAM_SPLINE_EVALUATE_OFFSET = 0x32460; // 0xDA2460 - 0xD70000
constexpr int CAM_REEVAL_INTERVAL = 30;
constexpr float PLAYER_CAM_SCORE = 20.0f;

// Spectator object layout:
// [0] = mode (0=fixed, 1=spline, 2=player, 3=replay)
// [3] = static cam count,  [4] = static cam cur index,  [5] = static cam array ptr
// [6] = dynamic cam count, [7] = dynamic cam cur index, [8] = dynamic cam array ptr

// Static camera entry: 32 bytes
struct StaticCamEntry {
    float x, y, z;
    float pitch, yaw;
    DWORD nameId;
    DWORD namePtr;
    float zoomDiv;
};

// Dynamic camera entry: 40 bytes
// [0] = spline pointer, [1] = 0, [2-3] = duration (repeated), [4] = flags,
// [5] = time/progress, [6] = pitch, [7] = yaw, [8] = nameId, [9] = zoomDiv
struct DynCamEntry {
    DWORD splinePtr;
    DWORD pad;
    float duration;
    float duration2;
    DWORD flags;
    float progress;
    float pitch;
    float yaw;
    DWORD nameId;
    float zoomDiv;
};

// Cached dynamic camera positions (resolved once via CAM_Spline_Evaluate)
static float g_dynCamPos[16][3]; // max 16 dynamic cams
static bool g_dynCamPosResolved = false;

static FILE* g_wcLog = nullptr;

static void LogWC(const char* fmt, ...) {
    if (!g_wcLog) {
        g_wcLog = fopen("worldcam_debug.log", "w");
    }
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_wcLog) {
        va_start(args, fmt);
        vfprintf(g_wcLog, fmt, args);
        va_end(args);
        fflush(g_wcLog);
    }
}

static bool HasLineOfSight(float* camPos, float* playerPos) {
    if (!OctCollision_IsLoaded()) return true;
    // Offset player position up to eye height to avoid ground clipping
    float targetPos[3] = { playerPos[0], playerPos[1], playerPos[2] + 0.5f };
    bool clear = OctCollision_LineOfSight(camPos, targetPos);
    LogWC("[WorldCam] LOS check: cam(%.1f,%.1f,%.1f)->player(%.1f,%.1f,%.1f) %s\n",
          camPos[0], camPos[1], camPos[2], targetPos[0], targetPos[1], targetPos[2],
          clear ? "CLEAR" : "BLOCKED");
    return clear;
}

static float DistanceSq(float* a, float* b) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return dx*dx + dy*dy + dz*dz;
}

// Resolve dynamic camera positions by evaluating their splines at time 0
static void ResolveDynCamPositions(int* spectObj) {
    if (g_dynCamPosResolved || !g_CAM_Spline_Evaluate) return;

    int dynCount = spectObj[6];
    uintptr_t dynArray = (uintptr_t)spectObj[8];
    if (dynCount <= 0 || dynArray == 0) return;

    g_dynCamPosResolved = true;
    LogWC("[WorldCam] Resolving %d dynamic camera positions via CAM_Spline_Evaluate...\n", dynCount);

    for (int i = 0; i < dynCount && i < 16; i++) {
        DynCamEntry* dyn = (DynCamEntry*)(dynArray + 40 * i);
        float splineOut[10] = {};
        if (dyn->splinePtr && g_CAM_Spline_Evaluate(dyn->splinePtr, 0.0f, splineOut, nullptr)) {
            g_dynCamPos[i][0] = splineOut[0];
            g_dynCamPos[i][1] = splineOut[1];
            g_dynCamPos[i][2] = splineOut[2];
            LogWC("  dyncam[%d]: pos=(%.2f, %.2f, %.2f) pitch=%.4f yaw=%.4f\n",
                  i, g_dynCamPos[i][0], g_dynCamPos[i][1], g_dynCamPos[i][2],
                  dyn->pitch, dyn->yaw);
        } else {
            g_dynCamPos[i][0] = g_dynCamPos[i][1] = g_dynCamPos[i][2] = 0;
            LogWC("  dyncam[%d]: spline eval FAILED (ptr=0x%08X)\n", i, dyn->splinePtr);
        }
    }
}

struct CamSearchResult {
    int index;
    int type; // 0=static, 1=dynamic
    float score;
};

static CamSearchResult FindBestCamera(int* spectObj, float* playerPos) {
    CamSearchResult best = { -1, 0, -1e30f };

    for (int type = 0; type < 2; type++) {
        int camCount = (type == 0) ? spectObj[3] : spectObj[6];
        if (camCount <= 0) continue;

        // For dynamic cams, need positions resolved
        if (type == 1 && !g_dynCamPosResolved) continue;

        for (int i = 0; i < camCount; i++) {
            float camPos[3];
            if (type == 0) {
                uintptr_t arr = (uintptr_t)spectObj[5];
                if (!arr) continue;
                StaticCamEntry* cam = (StaticCamEntry*)(arr + 32 * i);
                camPos[0] = cam->x; camPos[1] = cam->y; camPos[2] = cam->z;
            } else {
                if (i >= 16) continue;
                camPos[0] = g_dynCamPos[i][0];
                camPos[1] = g_dynCamPos[i][1];
                camPos[2] = g_dynCamPos[i][2];
                if (camPos[0] == 0 && camPos[1] == 0 && camPos[2] == 0) continue;
            }

            float distSq = DistanceSq(camPos, playerPos);
            if (distSq > MAX_CAM_DIST_SQ) continue;

            float dist = sqrtf(distSq);
            float score = 100.0f - (dist / MAX_CAM_DIST) * 80.0f;

            if (!HasLineOfSight(camPos, playerPos)) score -= 60.0f;

            // Slight stickiness bonus for current camera
            if (i == g_currentCamIndex && type == g_currentCamType) score += 5.0f;

            LogWC("[WorldCam]   %s[%d] d=%.1f score=%.1f\n",
                  type == 0 ? "static" : "dyncam", i, dist, score);

            if (score > best.score) {
                best = { i, type, score };
            }
        }
    }

    LogWC("[WorldCam] FindBest: player(%.1f,%.1f,%.1f) best=%s[%d] score=%.1f\n",
          playerPos[0], playerPos[1], playerPos[2],
          best.index >= 0 ? (best.type == 0 ? "static" : "dyncam") : "NONE",
          best.index, best.score);

    // Reject if best score is negative
    if (best.score < 0.0f) best.index = -1;
    return best;
}

static void ComputeAim(float* camPos, float* playerPos, float* outPitch, float* outYaw) {
    float dx = playerPos[0] - camPos[0];
    float dy = playerPos[1] - camPos[1];
    float dz = playerPos[2] - camPos[2];
    float horizDist = sqrtf(dx*dx + dy*dy);
    *outYaw = atan2f(-dx, dy);
    *outPitch = -atan2f(dz, horizDist);
}

static void* FindPlayerByHandle(uintptr_t baseGame, int handle) {
    void** playerTable = (void**)(baseGame + PLAYER_TABLE_OFFSET);
    for (int i = 0; i < 64; i++) {
        void* entry = playerTable[i];
        if (!entry) continue;
        if (*(int*)entry == handle) return entry;
    }
    return nullptr;
}

void InitWorldCameraTracker(uintptr_t baseGame) {
    g_baseGame = baseGame;
    g_CAM_Spline_Evaluate = (CAM_Spline_EvaluateFunc)(baseGame + CAM_SPLINE_EVALUATE_OFFSET);
    g_initialized = true;
    LogWC("[WorldCam] Initialized. CAM_Spline_Evaluate at 0x%08X\n", (uintptr_t)g_CAM_Spline_Evaluate);
}

void WorldCameraTracker_SetTarget(int playerHandle) {
    int prev = g_targetHandle.exchange(playerHandle);
    if (prev == playerHandle) return; // Same target, don't reset camera
    g_currentCamIndex = -1;
    g_currentCamType = 0;
    g_frameCounter = 0;
    g_holdFrames = 0;
    g_switchCooldownFrames = 0;
    LogWC("[WorldCam] Target set: handle=%d\n", playerHandle);
}

void WorldCameraTracker_ClearTarget() {
    g_targetHandle.store(0);
    g_currentCamIndex = -1;
    g_currentCamType = 0;
    g_frameCounter = 0;
    g_holdFrames = 0;
    g_switchCooldownFrames = 0;
}

int WorldCameraTracker_GetCurrentCamType() {
    if (g_targetHandle.load() == 0) return -1;
    return g_currentCamType;
}

bool WorldCameraTracker_GetCurrentCamPos(float* outPos) {
    if (g_currentCamIndex < 0) return false;

    // Need spectator object to read camera arrays — use game base to find it
    if (!g_baseGame) return false;
    int* spectObj = (int*)(g_baseGame + 0x7AE320);

    if (g_currentCamType == 0) {
        uintptr_t arr = (uintptr_t)spectObj[5];
        if (!arr) return false;
        StaticCamEntry* cam = (StaticCamEntry*)(arr + 32 * g_currentCamIndex);
        outPos[0] = cam->x;
        outPos[1] = cam->y;
        outPos[2] = cam->z;
        return true;
    } else if (g_currentCamType == 1) {
        if (g_currentCamIndex >= 16 || !g_dynCamPosResolved) return false;
        outPos[0] = g_dynCamPos[g_currentCamIndex][0];
        outPos[1] = g_dynCamPos[g_currentCamIndex][1];
        outPos[2] = g_dynCamPos[g_currentCamIndex][2];
        return true;
    }

    return false;
}

void WorldCameraTracker_Update(int* spectObj, uintptr_t baseGame) {
    if (!g_initialized) return;

    // Resolve dynamic cam positions once (needs spline data to be loaded)
    if (!g_dynCamPosResolved && spectObj[6] > 0) {
        ResolveDynCamPositions(spectObj);
    }

    int targetHandle = g_targetHandle.load();
    if (targetHandle == 0) return;

    int staticCount = spectObj[3];
    int dynamicCount = spectObj[6];
    if (staticCount <= 0 && dynamicCount <= 0) return;

    void* playerStruct = FindPlayerByHandle(baseGame, targetHandle);
    if (!playerStruct) {
        LogWC("[WorldCam] Player handle %d not found, clearing target\n", targetHandle);
        WorldCameraTracker_ClearTarget();
        return;
    }

    uintptr_t entityPtr = *(uintptr_t*)((uintptr_t)playerStruct + 244);
    if (entityPtr == 0) return;

    float playerPos[3];
    playerPos[0] = *(float*)(entityPtr + 0xD0);
    playerPos[1] = *(float*)(entityPtr + 0xD4);
    playerPos[2] = *(float*)(entityPtr + 0xD8);

    // Skip if position is at origin (not yet populated)
    if (playerPos[0] == 0.0f && playerPos[1] == 0.0f && playerPos[2] == 0.0f) return;

    g_frameCounter++;
    g_holdFrames++;
    if (g_switchCooldownFrames > 0) g_switchCooldownFrames--;

    bool needEval = (g_currentCamIndex == -1 && g_currentCamType != 2)
                  || (g_holdFrames >= MAX_HOLD_FRAMES)
                  || (g_switchCooldownFrames == 0 && (g_frameCounter % CAM_REEVAL_INTERVAL) == 0);

    if (needEval) {
        bool forceSwitch = (g_holdFrames >= MAX_HOLD_FRAMES);
        CamSearchResult result = FindBestCamera(spectObj, playerPos);

        if (result.index >= 0 && result.score > PLAYER_CAM_SCORE) {
            // World cam is good
            bool isNewCam = (result.index != g_currentCamIndex || result.type != g_currentCamType);
            if (isNewCam && (g_currentCamIndex == -1 || g_currentCamType == 2 || g_switchCooldownFrames == 0)) {
                g_currentCamIndex = result.index;
                g_currentCamType = result.type;
                g_switchCooldownFrames = SWITCH_COOLDOWN_FRAMES;
                g_holdFrames = 0;
                LogWC("[WorldCam] Selected %s cam %d score=%.1f (static=%d, dynamic=%d)\n",
                      g_currentCamType == 0 ? "static" : "dynamic",
                      g_currentCamIndex, result.score, staticCount, dynamicCount);
            } else if (forceSwitch) {
                // Max hold reached but same cam is still best — fall back to player cam for variety
                g_currentCamIndex = -1;
                g_currentCamType = 2;
                g_switchCooldownFrames = SWITCH_COOLDOWN_FRAMES;
                g_holdFrames = 0;
                spectObj[0] = 2;
                SpectCamUpdateFunc camUpdate = (SpectCamUpdateFunc)(g_baseGame + SPECT_CAM_UPDATE_OFFSET);
                camUpdate(spectObj);
                LogWC("[WorldCam] Max hold reached, same cam best — switching to player cam for variety\n");
            }
        } else {
            // Player cam is better (or no world cam at all)
            if (g_currentCamType != 2) {
                g_currentCamIndex = -1;
                g_currentCamType = 2;
                g_switchCooldownFrames = SWITCH_COOLDOWN_FRAMES;
                g_holdFrames = 0;
                spectObj[0] = 2;
                SpectCamUpdateFunc camUpdate = (SpectCamUpdateFunc)(g_baseGame + SPECT_CAM_UPDATE_OFFSET);
                camUpdate(spectObj);
                LogWC("[WorldCam] No good world cam (best score=%.1f), falling back to player camera\n",
                      result.score);
            } else if (forceSwitch) {
                // Already on player cam and max hold reached — re-evaluate next cycle
                g_holdFrames = 0;
            }
        }
    }

    // Player camera mode — no aim computation needed, just ensure mode is set
    if (g_currentCamType == 2) return;

    if (g_currentCamIndex < 0) return;

    // Set mode and index based on camera type
    // Mode 0 = static cameras (this[4] = index), Mode 1 = dynamic cameras (this[7] = index)
    int targetMode = g_currentCamType; // 0 for static, 1 for dynamic
    int modeIdxOffset = (g_currentCamType == 0) ? 4 : 7;

    bool modeChanged = (spectObj[0] != targetMode || spectObj[modeIdxOffset] != g_currentCamIndex);
    spectObj[0] = targetMode;
    spectObj[modeIdxOffset] = g_currentCamIndex;

    if (modeChanged) {
        SpectCamUpdateFunc camUpdate = (SpectCamUpdateFunc)(g_baseGame + SPECT_CAM_UPDATE_OFFSET);
        camUpdate(spectObj);
        LogWC("[WorldCam] Forced mode %d, %s cam %d\n", targetMode,
              g_currentCamType == 0 ? "static" : "dynamic", g_currentCamIndex);
    }

    // Compute aim from camera position to player
    float camPos[3];
    if (g_currentCamType == 0) {
        uintptr_t arr = (uintptr_t)spectObj[5];
        if (!arr) return;
        StaticCamEntry* cam = (StaticCamEntry*)(arr + 32 * g_currentCamIndex);
        camPos[0] = cam->x; camPos[1] = cam->y; camPos[2] = cam->z;

        float pitch, yaw;
        ComputeAim(camPos, playerPos, &pitch, &yaw);
        cam->pitch = pitch;
        cam->yaw = yaw;
    } else {
        // Dynamic cam: get position from cache, write pitch/yaw to entry
        if (g_currentCamIndex >= 16) return;
        camPos[0] = g_dynCamPos[g_currentCamIndex][0];
        camPos[1] = g_dynCamPos[g_currentCamIndex][1];
        camPos[2] = g_dynCamPos[g_currentCamIndex][2];

        uintptr_t dynArr = (uintptr_t)spectObj[8];
        if (!dynArr) return;
        DynCamEntry* dyn = (DynCamEntry*)(dynArr + 40 * g_currentCamIndex);

        float pitch, yaw;
        ComputeAim(camPos, playerPos, &pitch, &yaw);
        dyn->pitch = pitch;
        dyn->yaw = yaw;
    }
}
