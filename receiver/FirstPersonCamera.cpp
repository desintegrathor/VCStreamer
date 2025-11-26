#include "FirstPersonCamera.h"
#include "minhook/MinHook.h"
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <ctime>

// ============================================================================
// Constants
// ============================================================================

constexpr float EYE_OFFSET_STANDING = 0.55f;   // Eye height above chest when standing
constexpr float EYE_OFFSET_CROUCHING = 0.35f;  // Eye height above chest when crouching
constexpr int SPECTATOR_MODE_FREE = 2;         // Mode 2 = follow player

// Camera view modes
enum class CameraMode {
    ThirdPerson,    // 3PV - camera behind player
    FirstPerson     // FPV - camera at eye level (TODO: implement)
};

// Configuration
constexpr bool FPV_ENABLED = true;             // Enable random FPV/3PV selection
constexpr int FPV_CHANCE_PERCENT = 50;         // Chance of FPV when switching players (0-100)

// Game.dll offsets (IDA addresses minus IDA base 0xD40000)
constexpr uintptr_t OFFSET_FILL_CAMERA = 0x147650;           // IDA: 0xE87650
constexpr uintptr_t OFFSET_GET_CHEST_POS = 0x18BCB0;         // IDA: 0xECBCB0
constexpr uintptr_t OFFSET_SET_CAM_ROTATION_IMPORT = 0x1E7718; // IDA: 0xF27718
constexpr uintptr_t OFFSET_GET_BONE_LOC_IMPORT = 0x1E7E14;   // IDA: 0xF27E14 - SKE_GetBoneEndLoc
constexpr uintptr_t OFFSET_GET_EYE_POS_IMPORT = 0x1E7B04;    // IDA: 0xF27B04 - COM_MSHDEF_GetEyeWorldPos
constexpr uintptr_t OFFSET_GET_HAND_FIRE_IMPORT = 0x1E7B0C;  // IDA: 0xF27B0C - COM_MSHDEF_GetHandFireZone
constexpr uintptr_t OFFSET_CALC_FOLLOW_CAM = 0x12E570;       // IDA: 0xE6E570 - GPLAYER_CalculateFollowCameraPosition

// Structure offsets
constexpr int SPECTATOR_CTRL_MODE = 0x00;
constexpr int SPECTATOR_CTRL_CLIENT = 0x52C;
constexpr int CLIENT_PLAYER_ENTITY = 0xF4;
constexpr int PLAYER_STANCE_FLAG = 0x14;
constexpr int PLAYER_PITCH = 0xF4;
constexpr int PLAYER_YAW = 0xFC;
constexpr int PLAYER_SKELETON_PTR = 0x378;  // Pointer to skeleton array

// Bone IDs
constexpr unsigned int BONE_CHEST = 0x0B;        // 11 - Chest/Spine
constexpr unsigned int BONE_LEFT_SHOULDER = 0x0D;  // 13 - Left Shoulder
constexpr unsigned int BONE_RIGHT_SHOULDER = 0x12; // 18 - Right Shoulder
constexpr unsigned int BONE_HEAD = 0x17;         // 23 - Head (for camera position)
constexpr unsigned int BONE_WEAPON = 0x19;       // 25 - Weapon (for aim direction)

// ============================================================================
// Function types
// ============================================================================

// Original FillCamera function: int __thiscall GNET_SpectatorCtrl_FillCamera(void* this, void* cameraProp)
typedef int (__thiscall *FillCamera_t)(void* thisPtr, void* cameraProp);

// COM_I3DCamera_DirUp_From_RXYZ: void __cdecl (void* cameraProp, float pitch, float roll, float yaw)
typedef void (__cdecl *SetCameraRotation_t)(void* cameraProp, float pitch, float roll, float yaw);

// GPLAYER_GetChestPosition: float* __thiscall (void* playerEntity, float* outPos)
typedef float* (__thiscall *GetChestPosition_t)(void* playerEntity, float* outPos);

// SKE_GetBoneEndLoc: int __cdecl (void* skeleton, unsigned int boneId, float* outPos)
typedef int (__cdecl *GetBoneEndLoc_t)(void* skeleton, unsigned int boneId, float* outPos);

// COM_MSHDEF_GetEyeWorldPos: int __cdecl (void* skeleton, float* outPos)
// Returns 1 on success, 0 on failure
typedef int (__cdecl *GetEyeWorldPos_t)(void* skeleton, float* outPos);

// COM_MSHDEF_GetHandFireZone: void __cdecl (void* skeleton, float* outData)
// Returns 12 floats: [0-2] hand pos, [3-5] muzzle pos, [6-8] other hand, [9-11] other muzzle
typedef void (__cdecl *GetHandFireZone_t)(void* skeleton, float* outData);

// GPLAYER_CalculateFollowCameraPosition: int __cdecl (playerEntity, rotMatrix, outCamPos, outLookAt, distance)
// Calculates camera position with collision detection
typedef int (__cdecl *CalcFollowCameraPos_t)(void* playerEntity, float* rotMatrix, float* outCamPos, float* outLookAt, float distance);

// ============================================================================
// Global state
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

// Camera mode state
static CameraMode g_currentCameraMode = CameraMode::ThirdPerson;
static void* g_lastPlayerEntity = nullptr;
static bool g_randomInitialized = false;

// 3PV Yaw smoothing state
static float g_smoothedYaw = 0.0f;
static bool g_yawInitialized = false;

// FPV smoothing state
static float g_fpvSmoothedYaw = 0.0f;
static float g_fpvSmoothedPitch = 0.0f;
static bool g_fpvInitialized = false;

// ============================================================================
// Helper functions
// ============================================================================

// Normalize angle to [-PI, PI]
float NormalizeAngle(float angle) {
    while (angle > 3.14159265f) angle -= 6.28318530f;
    while (angle < -3.14159265f) angle += 6.28318530f;
    return angle;
}

// Lerp with angle wrapping
float LerpAngle(float from, float to, float t) {
    float diff = NormalizeAngle(to - from);
    return from + diff * t;
}

// Initialize random number generator
void InitRandom() {
    if (!g_randomInitialized) {
        srand(static_cast<unsigned int>(time(nullptr)));
        g_randomInitialized = true;
    }
}

// Select camera mode when player changes
void OnPlayerChanged(void* newPlayerEntity) {
    InitRandom();

    if (FPV_ENABLED) {
        // Random chance for FPV vs 3PV
        int roll = rand() % 100;
        if (roll < FPV_CHANCE_PERCENT) {
            g_currentCameraMode = CameraMode::FirstPerson;
            g_fpvInitialized = false;  // Reset FPV smoothing
            std::cout << "[Camera] Switched to FPV mode\n";
        } else {
            g_currentCameraMode = CameraMode::ThirdPerson;
            g_yawInitialized = false;  // Reset 3PV smoothing
            std::cout << "[Camera] Switched to 3PV mode\n";
        }
    } else {
        g_currentCameraMode = CameraMode::ThirdPerson;
        g_yawInitialized = false;
    }

    g_lastPlayerEntity = newPlayerEntity;
}

// Build 4x4 rotation matrix from yaw angle (rotation around Z axis)
void BuildRotationMatrix(float* m, float yaw) {
    float cy = cosf(yaw);
    float sy = sinf(yaw);

    // Row 0: X axis
    m[0] = cy;   m[1] = -sy;  m[2] = 0.0f;  m[3] = 0.0f;
    // Row 1: Y axis
    m[4] = sy;   m[5] = cy;   m[6] = 0.0f;  m[7] = 0.0f;
    // Row 2: Z axis
    m[8] = 0.0f; m[9] = 0.0f; m[10] = 1.0f; m[11] = 0.0f;
    // Row 3: Position/W
    m[12] = 0.0f; m[13] = 0.0f; m[14] = 0.0f; m[15] = 1.0f;
}

// Get bone rotation from skeleton
// Returns true if successful, fills outYaw and outPitch with Euler angles
bool GetBoneRotation(void* skeleton, unsigned int boneId, float* outYaw, float* outPitch) {
    // Skeleton structure:
    // +0x18 (+24) = bone count
    // +0x1C (+28) = bone array (pointer to array of bone pointers)

    unsigned int boneCount = *(unsigned int*)((char*)skeleton + 0x18);
    if (boneId >= boneCount) {
        return false;
    }

    void** boneArray = *(void***)((char*)skeleton + 0x1C);
    if (!boneArray) {
        return false;
    }

    void* bone = boneArray[boneId];
    if (!bone) {
        return false;
    }

    // Bone structure:
    // +0x1C (+28) = local transform matrix (4x4, 64 bytes)
    // +0x88 (+136) = world transform pointer (if not null, matrix is at worldTransform + 28)

    void* worldTransform = *(void**)((char*)bone + 0x88);
    float* matrix;
    if (worldTransform) {
        matrix = (float*)((char*)worldTransform + 0x1C);
    } else {
        matrix = (float*)((char*)bone + 0x1C);
    }

    // Matrix layout (row-major, 16 floats):
    // [0]  [1]  [2]  [3]   - X axis (right vector in bone space)
    // [4]  [5]  [6]  [7]   - Y axis (forward vector in bone space)
    // [8]  [9]  [10] [11]  - Z axis (up vector in bone space)
    // [12] [13] [14] [15]  - Position + w

    // For hips bone, Y axis (row 1) points forward in character space
    float fwdX = matrix[4];
    float fwdY = matrix[5];
    float fwdZ = matrix[6];

    // Yaw = rotation around world Z (up) axis
    // Returns raw yaw - caller adds offsets as needed
    *outYaw = atan2f(fwdX, fwdY);

    // Pitch from forward vector Z component (raw, no clamping)
    *outPitch = asinf(fwdZ);

    return true;
}

// ============================================================================
// Hook function
// ============================================================================

// Using __fastcall to handle __thiscall convention (thisPtr in ECX, ignore EDX)
int __fastcall Hooked_FillCamera(void* thisPtr, void* edx_unused, void* cameraProp) {
    // Check spectator mode - only modify Mode 2 (follow player)
    int mode = *(int*)((char*)thisPtr + SPECTATOR_CTRL_MODE);

    if (mode == SPECTATOR_MODE_FREE) {
        // Get camera entry index: this[10] = *(this + 0x28)
        int cameraIndex = *(int*)((char*)thisPtr + 0x28);

        // Get camera entry pointer: this + 44 + 20*index
        // Camera entry structure (20 bytes):
        //   +0: unknown
        //   +4: unknown
        //   +8: pitch (mouse Y input)
        //   +12: yaw (mouse X input)
        //   +16: unknown
        char* cameraEntry = (char*)thisPtr + 44 + 20 * cameraIndex;

        // Get client and player entity
        void* client = *(void**)((char*)thisPtr + SPECTATOR_CTRL_CLIENT);
        if (client) {
            void* playerEntity = *(void**)((char*)client + CLIENT_PLAYER_ENTITY);
            if (playerEntity) {
                // Detect player change
                if (playerEntity != g_lastPlayerEntity) {
                    OnPlayerChanged(playerEntity);
                }

                // Get skeleton
                void** skeletonPtrPtr = *(void***)((char*)playerEntity + PLAYER_SKELETON_PTR);
                if (skeletonPtrPtr && *skeletonPtrPtr) {
                    void* skeleton = *skeletonPtrPtr;

                    float yaw = 0.0f;
                    float pitch = 0.0f;

                    if (g_currentCameraMode == CameraMode::ThirdPerson) {
                        // =============================================================
                        // 3PV MODE (Third Person View)
                        // =============================================================
                        // Override camera entry yaw/pitch BEFORE original call
                        // This makes camera orbit to position behind player
                        // Original function handles collision detection with our rotation

                        if (GetBoneRotation(skeleton, BONE_HEAD, &yaw, &pitch)) {
                            // 3PV offset: camera BEHIND player (+PI), minus 20° correction
                            constexpr float YAW_OFFSET_3PV = 3.14159265f - 0.35f;
                            yaw += YAW_OFFSET_3PV;

                            // Clamp pitch to ±10° for 3PV
                            constexpr float MAX_PITCH_3PV = 0.175f;
                            if (pitch > MAX_PITCH_3PV) pitch = MAX_PITCH_3PV;
                            if (pitch < -MAX_PITCH_3PV) pitch = -MAX_PITCH_3PV;

                            // Initialize smoothed yaw on first use or player change
                            if (!g_yawInitialized || playerEntity != g_lastPlayerEntity) {
                                g_smoothedYaw = yaw;
                                g_yawInitialized = true;
                            }

                            // Smooth yaw interpolation (lower = slower/smoother)
                            constexpr float SMOOTH_FACTOR = 0.01f;
                            g_smoothedYaw = LerpAngle(g_smoothedYaw, yaw, SMOOTH_FACTOR);

                            // Override input values - original will use these for positioning
                            *(float*)(cameraEntry + 8) = pitch;         // clamped pitch
                            *(float*)(cameraEntry + 12) = g_smoothedYaw; // smoothed yaw with offset
                        }
                    } else if (g_currentCameraMode == CameraMode::FirstPerson) {
                        // =============================================================
                        // FPV MODE (First Person View)
                        // =============================================================
                        // Will be handled after original call
                    }
                }
            }
        }
    }

    // Call original function - it will use our overwritten yaw/pitch values
    int result = g_OriginalFillCamera(thisPtr, cameraProp);

    // Post-processing for FPV mode (needs to override position after original)
    if (mode == SPECTATOR_MODE_FREE && result && g_currentCameraMode == CameraMode::FirstPerson) {
        void* client = *(void**)((char*)thisPtr + SPECTATOR_CTRL_CLIENT);
        if (client) {
            void* playerEntity = *(void**)((char*)client + CLIENT_PLAYER_ENTITY);
            if (playerEntity) {
                void** skeletonPtrPtr = *(void***)((char*)playerEntity + PLAYER_SKELETON_PTR);
                if (skeletonPtrPtr && *skeletonPtrPtr) {
                    void* skeleton = *skeletonPtrPtr;

                    // Get eye position
                    float eyePos[3];
                    if (g_GetEyeWorldPos && g_GetEyeWorldPos(skeleton, eyePos)) {
                        float yaw = 0.0f, pitch = 0.0f;

                        // Get rotation from head bone
                        if (GetBoneRotation(skeleton, BONE_HEAD, &yaw, &pitch)) {
                            // FPV pitch: clamp to ±10° and offset slightly upward
                            constexpr float MAX_PITCH_FPV = 0.175f;  // 10 degrees
                            constexpr float PITCH_OFFSET_FPV = -0.09f; // ~5° upward (negative = up)
                            pitch += PITCH_OFFSET_FPV;
                            if (pitch > MAX_PITCH_FPV) pitch = MAX_PITCH_FPV;
                            if (pitch < -MAX_PITCH_FPV) pitch = -MAX_PITCH_FPV;

                            // Initialize FPV smoothing on first use or player change
                            if (!g_fpvInitialized || playerEntity != g_lastPlayerEntity) {
                                g_fpvSmoothedYaw = yaw;
                                g_fpvSmoothedPitch = pitch;
                                g_fpvInitialized = true;
                            }

                            // FPV smoothing (0.03 = smooth, 3PV uses 0.01)
                            constexpr float FPV_SMOOTH_FACTOR = 0.03f;
                            g_fpvSmoothedYaw = LerpAngle(g_fpvSmoothedYaw, yaw, FPV_SMOOTH_FACTOR);
                            g_fpvSmoothedPitch = g_fpvSmoothedPitch + (pitch - g_fpvSmoothedPitch) * FPV_SMOOTH_FACTOR;

                            // Set camera at eye position
                            float* camPos = (float*)cameraProp;
                            camPos[0] = eyePos[0];
                            camPos[1] = eyePos[1];
                            camPos[2] = eyePos[2];

                            if (g_SetCameraRotation) {
                                g_SetCameraRotation(cameraProp, g_fpvSmoothedPitch, 0.0f, g_fpvSmoothedYaw);
                            }
                        }
                    }
                }
            }
        }
    }

    return result;
}

// ============================================================================
// Public functions
// ============================================================================

bool InitFirstPersonCamera(uintptr_t gameBase) {
    g_gameBase = gameBase;

    std::cout << "[FirstPerson] Initializing first-person camera hook...\n";
    std::cout << "[FirstPerson] game.dll base: 0x" << std::hex << gameBase << std::dec << "\n";

    // Initialize MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        std::cout << "[FirstPerson] Failed to initialize MinHook: " << MH_StatusToString(status) << "\n";
        return false;
    }

    // Resolve function addresses
    uintptr_t fillCameraAddr = gameBase + OFFSET_FILL_CAMERA;
    g_GetChestPosition = (GetChestPosition_t)(gameBase + OFFSET_GET_CHEST_POS);
    g_CalcFollowCamPos = (CalcFollowCameraPos_t)(gameBase + OFFSET_CALC_FOLLOW_CAM);

    // Read imported functions from import table (they are pointers to functions)
    g_SetCameraRotation = *(SetCameraRotation_t*)(gameBase + OFFSET_SET_CAM_ROTATION_IMPORT);
    g_GetBoneEndLoc = *(GetBoneEndLoc_t*)(gameBase + OFFSET_GET_BONE_LOC_IMPORT);
    g_GetEyeWorldPos = *(GetEyeWorldPos_t*)(gameBase + OFFSET_GET_EYE_POS_IMPORT);
    g_GetHandFireZone = *(GetHandFireZone_t*)(gameBase + OFFSET_GET_HAND_FIRE_IMPORT);

    std::cout << "[FirstPerson] FillCamera addr: 0x" << std::hex << fillCameraAddr << "\n";
    std::cout << "[FirstPerson] CalcFollowCamPos addr: 0x" << (uintptr_t)g_CalcFollowCamPos << "\n";
    std::cout << "[FirstPerson] GetBoneEndLoc addr: 0x" << (uintptr_t)g_GetBoneEndLoc << "\n";
    std::cout << "[FirstPerson] GetEyeWorldPos addr: 0x" << (uintptr_t)g_GetEyeWorldPos << "\n";
    std::cout << "[FirstPerson] SetCamRotation addr: 0x" << (uintptr_t)g_SetCameraRotation << std::dec << "\n";

    // Create hook
    status = MH_CreateHook(
        (LPVOID)fillCameraAddr,
        (LPVOID)&Hooked_FillCamera,
        (LPVOID*)&g_OriginalFillCamera
    );

    if (status != MH_OK) {
        std::cout << "[FirstPerson] Failed to create hook: " << MH_StatusToString(status) << "\n";
        return false;
    }

    // Enable hook
    status = MH_EnableHook((LPVOID)fillCameraAddr);
    if (status != MH_OK) {
        std::cout << "[FirstPerson] Failed to enable hook: " << MH_StatusToString(status) << "\n";
        MH_RemoveHook((LPVOID)fillCameraAddr);
        return false;
    }

    g_hookInstalled = true;
    std::cout << "[FirstPerson] Camera hook installed successfully!\n";
    std::cout << "[FirstPerson] First-person mode is now active in spectator Mode 2\n";

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
