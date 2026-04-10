#define _CRT_SECURE_NO_WARNINGS
#include "FpvViewmodel.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <tlhelp32.h>
#include "minhook/MinHook.h"

// ============================================================================
// Logging
// ============================================================================

static FILE* g_fpvmLog = nullptr;

static void FpvmLog(const char* fmt, ...) {
    if (!g_fpvmLog) {
        g_fpvmLog = fopen("autospectator_debug.log", "a");
        if (!g_fpvmLog) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_fpvmLog, fmt, args);
    fflush(g_fpvmLog);
    va_end(args);
}

// ============================================================================
// Game.dll offsets
// Data globals: RVA = IDA_addr - 0xD70000
// Functions:    RVA = IDA_addr - 0xD40000
// ============================================================================

// Data globals (IDA - 0xD70000)
constexpr uintptr_t RVA_LOCAL_PLAYER       = 0x80D458;  // g_localPlayer (IDA 0x157D458)
constexpr uintptr_t RVA_THIRD_PERSON       = 0x77B12C;  // g_thirdPersonEnabled (IDA 0x14E812C)
constexpr uintptr_t RVA_FPV_HANDS_NODE     = 0x803658;  // g_fpvHandsNode (IDA 0x1573658)
constexpr uintptr_t RVA_FPV_HANDS_SKELETON = 0x80365C;  // g_fpvHandsSkeleton (IDA 0x157365C)
constexpr uintptr_t RVA_FPV_ANIM_LAYER_CNT = 0x8036CC;  // g_fpvAnimLayerCount (IDA 0x15736CC)
constexpr uintptr_t RVA_FPV_EXTRA_ANIM_CNT = 0x803994;  // g_fpvExtraAnimCount (IDA 0x1573994)
constexpr uintptr_t RVA_FPV_WEAPON_ALPHA   = 0x803668;  // g_fpvWeaponAlpha (IDA 0x1573668)
constexpr uintptr_t RVA_FPV_ANIM_MODE      = 0x80366C;  // g_fpvAnimMode (IDA 0x157366C)

constexpr uintptr_t RVA_FPV_STATE_OBJ      = 0x803618;  // g_fpvStateObj (IDA 0x1573618) — "this" for FPV_StartAnim
constexpr uintptr_t RVA_ACTIVE_WEAPON_OBJ  = 0x830114;  // g_activeWeaponObj (IDA 0x15A0114)

// Functions (IDA - 0xD70000, same as data — verified by AOB scan)
constexpr uintptr_t RVA_RENDER_WEAPON      = 0x1AEBA0;  // GAM_FPV_RenderWeapon (IDA 0xF1EBA0)
constexpr uintptr_t RVA_START_ANIM         = 0x1AC350;  // FPV_StartAnim (IDA 0xF1C350)

// IAT import pointers
constexpr uintptr_t RVA_REN_RENDER_IAT     = 0x1E7B10;  // REN_Render_Nod_Iske
constexpr uintptr_t RVA_XBES_ADD_SCENE_IAT = 0x1E7D3C;  // XBES_Add_Scene

// ============================================================================
// Function types
// ============================================================================

typedef void (__cdecl *FpvRenderWeapon_t)(int is_mirror_setup, int is_mirror_render);

// FPV_StartAnim is __thiscall: double FPV_StartAnim(void* this, unsigned int anim_type, DWORD* out_anim_id)
// We use a raw call via inline asm or cast to __thiscall
typedef double (__thiscall *FpvStartAnim_t)(void* thisPtr, unsigned int anim_type, DWORD* out_anim_id);

// REN_Render_Nod_Iske(s_RenderBuffer_nod_iske* buf, unsigned int flags)
typedef void (__cdecl *RenRenderNodIske_t)(void* renderBuf, unsigned int flags);

// XBES_Add_Scene(s_NOD* node) — adds a BES node to the render scene graph
typedef unsigned long (__cdecl *XbesAddScene_t)(void* node);


// ============================================================================
// Resolved pointers
// ============================================================================

static uintptr_t g_gameBase = 0;
static bool g_initialized = false;

static DWORD** g_ppLocalPlayer = nullptr;
static DWORD*  g_pThirdPerson = nullptr;

static DWORD*  g_pFpvHandsNode = nullptr;
static DWORD*  g_pFpvHandsSkeleton = nullptr;
static DWORD*  g_pFpvAnimLayerCount = nullptr;
static DWORD*  g_pFpvExtraAnimCount = nullptr;
static float*  g_pFpvWeaponAlpha = nullptr;
static DWORD*  g_pFpvAnimMode = nullptr;

static FpvRenderWeapon_t g_pfnRenderWeapon = nullptr;
static FpvRenderWeapon_t g_origRenderWeapon = nullptr;
static FpvStartAnim_t g_pfnStartAnim = nullptr;
static void* g_pFpvStateObj = nullptr;
static DWORD* g_pActiveWeaponObj = nullptr;
static RenRenderNodIske_t g_pfnRenRender = nullptr;
static XbesAddScene_t g_pfnAddScene = nullptr;
static bool g_hookInstalledOnRender = false;
static bool g_handsAddedToScene = false;
static void* g_currentSpectEntity = nullptr;

// ============================================================================
// Shadow entity buffer
// ============================================================================

static constexpr size_t SHADOW_DWORD_COUNT = 1024;
static DWORD g_shadowEntity[SHADOW_DWORD_COUNT];

static BYTE g_healthStub[16];
static BYTE g_stub414[3080];

// ============================================================================
// State tracking
// ============================================================================

static void*  g_lastSpectatedEntity = nullptr;
static DWORD  g_lastWeaponPtr = 0;
static DWORD  g_mainThreadId = 0;
static int    g_skipFrames = 0;  // skip N frames after target switch

// ============================================================================
// Thread suspension for safe g_localPlayer swap
// ============================================================================

static DWORD g_suspendedThreads[64];
static int g_suspendedCount = 0;

static void SuspendOtherThreads() {
    g_suspendedCount = 0;
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != tid) {
                HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (ht) {
                    SuspendThread(ht);
                    CloseHandle(ht);
                    if (g_suspendedCount < 64)
                        g_suspendedThreads[g_suspendedCount++] = te.th32ThreadID;
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void ResumeOtherThreads() {
    for (int i = 0; i < g_suspendedCount; i++) {
        HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME, FALSE, g_suspendedThreads[i]);
        if (ht) {
            ResumeThread(ht);
            CloseHandle(ht);
        }
    }
    g_suspendedCount = 0;
}

// Forward declaration
static void __cdecl Hooked_RenderWeapon(int is_mirror_setup, int is_mirror_render);

// ============================================================================
// Init / Shutdown
// ============================================================================

bool InitFpvViewmodel(uintptr_t gameBase) {
    g_gameBase = gameBase;
    g_mainThreadId = GetCurrentThreadId();

    g_ppLocalPlayer     = (DWORD**)(gameBase + RVA_LOCAL_PLAYER);
    g_pThirdPerson      = (DWORD*)(gameBase + RVA_THIRD_PERSON);

    // Patch out GAM_Entity_GetType check in GAM_FPV_RenderWeapon.
    // IDA 0xF1EBC5..0xF1EBDB (23 bytes): mov ecx/call/cmp/jnz
    // This check reads a global object that's not valid in spectator mode.
    {
        uintptr_t patchAddr = gameBase + 0x1AEBC5; // IDA 0xF1EBC5 - 0xD70000
        BYTE* p = (BYTE*)patchAddr;
        // Verify first bytes: B9 (mov ecx, imm32)
        if (p[0] == 0xB9) {
            DWORD oldProt;
            if (VirtualProtect((void*)patchAddr, 23, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memset((void*)patchAddr, 0x90, 23); // NOP 23 bytes
                VirtualProtect((void*)patchAddr, 23, oldProt, &oldProt);
                FpvmLog("[FpvViewmodel] Patched entity type check at 0x%08X (23 NOPs)\n", patchAddr);
            }
        } else {
            FpvmLog("[FpvViewmodel] Entity type patch: expected B9 at 0x%08X, got %02X — NOT patching\n", patchAddr, p[0]);
        }
    }
    g_pFpvHandsNode     = (DWORD*)(gameBase + RVA_FPV_HANDS_NODE);
    g_pFpvHandsSkeleton = (DWORD*)(gameBase + RVA_FPV_HANDS_SKELETON);
    g_pFpvAnimLayerCount = (DWORD*)(gameBase + RVA_FPV_ANIM_LAYER_CNT);
    g_pFpvExtraAnimCount = (DWORD*)(gameBase + RVA_FPV_EXTRA_ANIM_CNT);
    g_pFpvWeaponAlpha   = (float*)(gameBase + RVA_FPV_WEAPON_ALPHA);
    g_pFpvAnimMode      = (DWORD*)(gameBase + RVA_FPV_ANIM_MODE);

    g_pfnRenderWeapon = (FpvRenderWeapon_t)(gameBase + RVA_RENDER_WEAPON);
    g_pfnStartAnim    = (FpvStartAnim_t)(gameBase + RVA_START_ANIM);
    g_pFpvStateObj    = (void*)(gameBase + RVA_FPV_STATE_OBJ);
    g_pActiveWeaponObj = (DWORD*)(gameBase + RVA_ACTIVE_WEAPON_OBJ);

    // Resolve imports from IAT
    g_pfnRenRender = *(RenRenderNodIske_t*)(gameBase + RVA_REN_RENDER_IAT);
    g_pfnAddScene  = *(XbesAddScene_t*)(gameBase + RVA_XBES_ADD_SCENE_IAT);

    memset(g_healthStub, 0, sizeof(g_healthStub));
    *(float*)(g_healthStub + 8) = 100.0f;

    memset(g_stub414, 0, sizeof(g_stub414));
    *(DWORD*)(g_stub414 + 3076) = 99;

    memset(g_shadowEntity, 0, sizeof(g_shadowEntity));

    // Hook GAM_FPV_RenderWeapon so we run at the right pipeline stage
    {
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
            FpvmLog("[FpvViewmodel] MinHook init failed\n");
            return false;
        }
        st = MH_CreateHook((void*)g_pfnRenderWeapon, (void*)&Hooked_RenderWeapon, (void**)&g_origRenderWeapon);
        if (st != MH_OK) {
            FpvmLog("[FpvViewmodel] Failed to hook RenderWeapon: %d\n", st);
            return false;
        }
        st = MH_EnableHook((void*)g_pfnRenderWeapon);
        if (st != MH_OK) {
            FpvmLog("[FpvViewmodel] Failed to enable RenderWeapon hook: %d\n", st);
            return false;
        }
        g_hookInstalledOnRender = true;
        FpvmLog("[FpvViewmodel] Hooked GAM_FPV_RenderWeapon at 0x%08X\n", (uintptr_t)g_pfnRenderWeapon);
    }

    g_initialized = true;
    FpvmLog("[FpvViewmodel] Initialized, gameBase=0x%08X\n", gameBase);
    return true;
}

void ShutdownFpvViewmodel() {
    g_initialized = false;
    if (g_fpvmLog) {
        fclose(g_fpvmLog);
        g_fpvmLog = nullptr;
    }
}

// ============================================================================
// Hooked GAM_FPV_RenderWeapon — called by the game from GAM_DoTick
// at exactly the right point in the render pipeline.
// ============================================================================

static void __cdecl Hooked_RenderWeapon(int is_mirror_setup, int is_mirror_render) {
    // Only intercept the (0,1) call from GAM_DoTick — skip scene callback (0,0) which uses SPO_Walker
    if (!g_initialized || !g_currentSpectEntity || !is_mirror_render) {
        g_origRenderWeapon(is_mirror_setup, is_mirror_render);
        return;
    }

    DWORD* spectEnt = (DWORD*)g_currentSpectEntity;

    DWORD activeWeapPtr = spectEnt[274];
    if (activeWeapPtr == 0 || activeWeapPtr < 0x10000 || (spectEnt[50] & 0x20000)) {
        g_origRenderWeapon(is_mirror_setup, is_mirror_render);
        return;
    }

    if (*g_pFpvHandsNode == 0 || *g_pFpvHandsSkeleton == 0) {
        return; // no hands loaded
    }

    // --- Detect target switch ---
    if (g_currentSpectEntity != g_lastSpectatedEntity) {
        FpvmLog("[FpvViewmodel] Target switch: entity=0x%08X\n", (uintptr_t)g_currentSpectEntity);
        g_lastSpectatedEntity = g_currentSpectEntity;
        g_lastWeaponPtr = 0;
        g_skipFrames = 2;  // skip 2 frames to let state settle
        *g_pFpvAnimLayerCount = 0;
        *g_pFpvExtraAnimCount = 0;
        *g_pFpvWeaponAlpha = 1.0f;
        *g_pFpvAnimMode = 0;
    }

    if (g_skipFrames > 0) {
        g_skipFrames--;
        g_origRenderWeapon(is_mirror_setup, is_mirror_render);
        return;
    }

    // --- Populate shadow entity ---
    memset(g_shadowEntity, 0, sizeof(g_shadowEntity));
    g_shadowEntity[1]   = 3;
    g_shadowEntity[5]   = 0;
    g_shadowEntity[24]  = 1;
    // Yaw/pitch: FPV renderer uses player[30] as yaw, player[29] as pitch
    // Remote entity yaw is inverted vs local player convention — add PI to flip
    {
        float yaw = *(float*)&spectEnt[30];
        float pitch = *(float*)&spectEnt[29];
        yaw += 3.14159265f;
        *(float*)&g_shadowEntity[30] = yaw;
        *(float*)&g_shadowEntity[29] = pitch;
    }
    g_shadowEntity[50]  = 0;
    // Look direction: same PI offset for yaw
    {
        float lookYaw = *(float*)&spectEnt[63];
        float lookPitch = *(float*)&spectEnt[61];
        lookYaw += 3.14159265f;
        *(float*)&g_shadowEntity[63] = lookYaw;
        *(float*)&g_shadowEntity[61] = lookPitch;
    }
    g_shadowEntity[180] = (DWORD)g_healthStub;
    g_shadowEntity[274] = activeWeapPtr;
    g_shadowEntity[414] = (DWORD)g_stub414;
    g_shadowEntity[707] = 0;
    // shadow[792] (SPO_Walker) left as 0 — not needed for mirror render path

    // --- Swap globals (no thread suspend needed — we're on the main game thread) ---
    DWORD* savedLocalPlayer = *g_ppLocalPlayer;
    DWORD  savedThirdPerson = *g_pThirdPerson;
    DWORD  savedActiveWeapon = *g_pActiveWeaponObj;

    *g_ppLocalPlayer = g_shadowEntity;
    *g_pThirdPerson = 1;
    *g_pActiveWeaponObj = activeWeapPtr;  // point to spectated player's weapon

    // --- Re-add hands node to scene if needed ---
    if (!g_handsAddedToScene && g_pfnAddScene && *g_pFpvHandsNode) {
        __try {
            g_pfnAddScene((void*)*g_pFpvHandsNode);
            g_handsAddedToScene = true;
            FpvmLog("[FpvViewmodel] Re-added hands node 0x%08X to scene\n", *g_pFpvHandsNode);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            FpvmLog("[FpvViewmodel] CRASH adding hands to scene\n");
        }
    }

    // --- Init animation on weapon change ---
    if (activeWeapPtr != g_lastWeaponPtr) {
        __try {
            g_pfnStartAnim(g_pFpvStateObj, 1, nullptr);  // RESET
            g_pfnStartAnim(g_pFpvStateObj, 2, nullptr);  // WALK (visible weapon hold)
            g_lastWeaponPtr = activeWeapPtr;
            FpvmLog("[FpvViewmodel] StartAnim OK, animLayers=%d\n", *g_pFpvAnimLayerCount);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            FpvmLog("[FpvViewmodel] CRASH in StartAnim!\n");
            g_lastWeaponPtr = activeWeapPtr;
        }
    }

    // --- Call original render (sets up projection, positions skeleton, etc.) ---
    __try {
        g_origRenderWeapon(is_mirror_setup, is_mirror_render);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_crashLog = 0;
        if (s_crashLog < 3) {
            FpvmLog("[FpvViewmodel] Render crash (args=%d,%d)\n", is_mirror_setup, is_mirror_render);
            s_crashLog++;
        }
    }

    // Direct REN_Render removed — game's own RenderWeapon path works

    // --- Restore ---
    *g_ppLocalPlayer = savedLocalPlayer;
    *g_pThirdPerson = savedThirdPerson;
    *g_pActiveWeaponObj = savedActiveWeapon;

    static int s_okCount = 0;
    if (s_okCount < 10) {
        FpvmLog("[FpvViewmodel] Render OK! args=(%d,%d) alpha=%.2f layers=%d\n",
            is_mirror_setup, is_mirror_render,
            *g_pFpvWeaponAlpha, *g_pFpvAnimLayerCount);
        s_okCount++;
    }
}

// ============================================================================
// FpvViewmodel_RenderFrame — called from Hooked_FillCamera each frame.
// Just stores the entity pointer for the hook to use.
// ============================================================================

bool FpvViewmodel_RenderFrame(void* playerEntity) {
    g_currentSpectEntity = playerEntity;
    return g_initialized && playerEntity != nullptr;
}
