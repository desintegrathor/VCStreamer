#include "FirstPersonCamera.h"
#include "CameraDirector.h"
#include "DelayManager.h"
#include "WorldCameraTracker.h"
#include "minhook/MinHook.h"
#include <iostream>
#include <cmath>
#include <cstring>

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

// Bone IDs
constexpr unsigned int BONE_HEAD = 0x17;

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

// KillCam render-side position tracking for slide interpolation
static float g_killCamKillerLastPos[3] = {};
static bool g_killCamKillerPosLocked = false;
static float g_killCamVictimLastPos[3] = {};
static bool g_killCamVictimPosLocked = false;
static bool g_killCamPhase2Entered = false;

// Track last director state for detecting KillCam entry
static CameraState g_lastDirState = CameraState::Idle;

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

static void GetEntityPos(void* entity, float* out) {
    out[0] = *(float*)((uintptr_t)entity + 0xD0);
    out[1] = *(float*)((uintptr_t)entity + 0xD4);
    out[2] = *(float*)((uintptr_t)entity + 0xD8);
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

int __fastcall Hooked_FillCamera(void* thisPtr, void* edx_unused, void* cameraProp) {
    DelayManager::ReloadConfigIfNeeded();

    int mode = *(int*)((char*)thisPtr + SPECTATOR_CTRL_MODE);

    if (mode != SPECTATOR_MODE_FREE) {
        return g_OriginalFillCamera(thisPtr, cameraProp);
    }

    CameraState dirState = CameraDirector_GetState();
    KillCamPhase kcPhase = CameraDirector_GetKillCamPhase();
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
    }

    // Detect KillCam start (transition from non-KillCam to KillCam)
    if (dirState == CameraState::KillCam && g_lastDirState != CameraState::KillCam) {
        // New KillCam started — snapshot positions
        int killerHandle = CameraDirector_GetKillCamKillerHandle();
        int victimHandle = CameraDirector_GetKillCamVictimHandle();

        void* ke = FindEntityByHandle(killerHandle);
        if (ke) GetEntityPos(ke, g_killCamKillerLastPos);
        else memset(g_killCamKillerLastPos, 0, sizeof(g_killCamKillerLastPos));

        void* ve = FindEntityByHandle(victimHandle);
        if (ve) GetEntityPos(ve, g_killCamVictimLastPos);
        else memset(g_killCamVictimLastPos, 0, sizeof(g_killCamVictimLastPos));

        g_killCamKillerPosLocked = false;
        g_killCamVictimPosLocked = false;
        g_killCamPhase2Entered = false;
    }

    // 3PV pre-processing: run for all states except KillCam Transition
    bool do3pvPreprocess = !(dirState == CameraState::KillCam && kcPhase == KillCamPhase::Transition);

    if (do3pvPreprocess && playerEntity) {
        void** skeletonPtrPtr = *(void***)((char*)playerEntity + PLAYER_SKELETON_PTR);
        if (skeletonPtrPtr && *skeletonPtrPtr) {
            void* skeleton = *skeletonPtrPtr;
            float yaw = 0.0f;
            float pitch = 0.0f;

            if (GetBoneRotation(skeleton, BONE_HEAD, &yaw, &pitch)) {
                constexpr float YAW_OFFSET_3PV = 3.14159265f - 0.35f;
                yaw += YAW_OFFSET_3PV;

                constexpr float MAX_PITCH_3PV = 0.175f;
                if (pitch > MAX_PITCH_3PV) pitch = MAX_PITCH_3PV;
                if (pitch < -MAX_PITCH_3PV) pitch = -MAX_PITCH_3PV;

                if (!g_yawInitialized) {
                    g_smoothedYaw = yaw;
                    g_yawInitialized = true;
                }

                g_smoothedYaw = LerpAngle(g_smoothedYaw, yaw, cfg.tpvYawSmoothFactor);

                int cameraIndex = *(int*)((char*)thisPtr + 0x28);
                char* cameraEntry = (char*)thisPtr + 44 + 20 * cameraIndex;
                *(float*)(cameraEntry + 8) = pitch;
                *(float*)(cameraEntry + 12) = g_smoothedYaw;
            }
        }
    }

    // Call original function
    int result = g_OriginalFillCamera(thisPtr, cameraProp);

    // Post-processing: distance smoothing (skip during KillCam Transition)
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
            float t = SmoothStep((elapsed - waitEnd) / (transEnd - waitEnd));
            float* camPos = (float*)cameraProp;

            camPos[0] = killerPos[0] + (victimPos[0] - killerPos[0]) * t;
            camPos[1] = killerPos[1] + (victimPos[1] - killerPos[1]) * t;
            camPos[2] = killerPos[2] + (victimPos[2] - killerPos[2]) * t + cfg.killCamSlideHeight;

            // Look forward: killer -> victim direction
            float ddx = victimPos[0] - killerPos[0];
            float ddy = victimPos[1] - killerPos[1];
            float ddz = victimPos[2] - killerPos[2];
            float horizDist = sqrtf(ddx * ddx + ddy * ddy);

            float yaw = (horizDist > 0.01f) ? atan2f(-ddx, ddy) : 0.0f;
            float pitch = (horizDist > 0.01f) ? -atan2f(ddz, horizDist) : 0.0f;

            if (g_SetCameraRotation) {
                g_SetCameraRotation(cameraProp, pitch, 0.0f, yaw);
            }
        }

        // Phase 2 (attached): switch to victim on first frame
        if (kcPhase == KillCamPhase::Attached && !g_killCamPhase2Entered) {
            g_killCamPhase2Entered = true;

            // Switch spectator target to victim
            uintptr_t spectObj = g_gameBase + 0x7AE320;
            int playerCount = *(int*)(spectObj + 0x24);
            uintptr_t listBase = spectObj + 0x2C;
            for (int i = 0; i < playerCount && i < 64; i++) {
                if (*(int*)(listBase + i * 20) == victimHandle) {
                    *(int*)(spectObj + 0x28) = i;
                    break;
                }
            }

            // Reset smoothing for fresh 3PV on victim
            g_yawInitialized = false;
            g_distanceInitialized = false;

            std::cout << "[Camera] KillCam phase 2: attached to victim\n";
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
    if (g_hookInstalled) {
        std::cout << "[FirstPerson] Shutting down...\n";
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_hookInstalled = false;
    }
}
