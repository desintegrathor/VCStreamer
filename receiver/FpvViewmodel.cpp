#define _CRT_SECURE_NO_WARNINGS
#include "FpvViewmodel.h"
#include "DiagnosticsLog.h"
#include "minhook/MinHook.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <windows.h>

static FILE* g_fpvmLog = nullptr;
static void FpvmLog(const char* fmt, ...) {
    if (!DiagnosticsLog_IsEnabled()) return;

    if (!g_fpvmLog) {
        g_fpvmLog = fopen("autospectator_debug.log", "a");
        if (!g_fpvmLog) return;
    }
    va_list args;
    va_start(args, fmt);
    DiagnosticsLog_Write(g_fpvmLog, fmt, args);
    va_end(args);
}

// ============================================================================
// IAT imports (RVA = IDA - 0xD70000, resolved via gameBase + RVA)
// ============================================================================

constexpr uintptr_t RVA_BES_LOAD_IAT        = 0x1E8010;
constexpr uintptr_t RVA_XBES_ADD_SCENE_IAT  = 0x1E7D3C;
constexpr uintptr_t RVA_BES_FIRST_ISKE_IAT  = 0x1E73E4;
constexpr uintptr_t RVA_ISKE_SKELET_SET_IAT = 0x1E7E08;
constexpr uintptr_t RVA_REN_RENDER_IAT      = 0x1E7B10;
constexpr uintptr_t RVA_DELTA_TIME          = 0x777714;
constexpr uintptr_t RVA_HEAP                = 0x77774C;

typedef void*  (__cdecl *BesLoad_t)(const char*, unsigned long, void*);
typedef unsigned long (__cdecl *XbesAddScene_t)(void*);
typedef void*  (__cdecl *BesGetFirstIske_t)(void*);
typedef void   (__cdecl *IskeSkeletSet_t)(void*, float);
typedef void   (__cdecl *RenRenderNodIske_t)(void*, unsigned int);
typedef void   (__cdecl *FpvRenderWeapon_t)(int, int);

static uintptr_t g_gameBase = 0;
static bool g_initialized = false;
static float*  g_pDeltaTime = nullptr;
static HANDLE* g_pHeap = nullptr;

static BesLoad_t          g_pfnBesLoad = nullptr;
static XbesAddScene_t     g_pfnAddScene = nullptr;
static BesGetFirstIske_t  g_pfnGetFirstIske = nullptr;
static IskeSkeletSet_t    g_pfnIskeSkeletSet = nullptr;
static RenRenderNodIske_t g_pfnRenRender = nullptr;
static FpvRenderWeapon_t  g_origRenderWeapon = nullptr;

constexpr uintptr_t RVA_LOCAL_PLAYER       = 0x80D458;
constexpr uintptr_t RVA_THIRD_PERSON       = 0x77B12C;
constexpr uintptr_t RVA_FPV_HANDS_NODE_G   = 0x803658;
constexpr uintptr_t RVA_FPV_HANDS_SKEL_G   = 0x80365C;
constexpr uintptr_t RVA_FPV_WEAPON_ALPHA   = 0x803668;
constexpr uintptr_t RVA_FPV_ANIM_MODE      = 0x80366C;
constexpr uintptr_t RVA_FPV_ANIM_LAYER_CNT = 0x8036CC;
constexpr uintptr_t RVA_FPV_EXTRA_ANIM_CNT = 0x803994;

static DWORD** g_ppLocalPlayer = nullptr;
static DWORD*  g_pThirdPerson = nullptr;
static DWORD*  g_pFpvHandsNodeG = nullptr;
static DWORD*  g_pFpvHandsSkelG = nullptr;
static float*  g_pFpvWeaponAlpha = nullptr;
static DWORD*  g_pFpvAnimMode = nullptr;
static DWORD*  g_pFpvAnimLayerCnt = nullptr;
static DWORD*  g_pFpvExtraAnimCnt = nullptr;

static DWORD g_shadowEntity[1024];
static BYTE g_healthStub[16];
static BYTE g_stub414[3080];

static void* g_handsNode = nullptr;
static void* g_handsSkel = nullptr;
static bool  g_handsLoaded = false;
static bool  g_wantRender = false;
static void* g_spectatedEntity = nullptr;

// Forward declaration
static void __cdecl Hooked_RenderWeapon(int, int);

// ============================================================================
// Load hands mesh
// ============================================================================

static bool LoadHands() {
    if (g_handsLoaded) return true;
    __try {
        g_handsNode = g_pfnBesLoad("g\\characters\\hands\\us\\HANDS_US.BES", 8, nullptr);
        if (!g_handsNode) return false;
        g_pfnAddScene(g_handsNode);
        g_handsSkel = g_pfnGetFirstIske(g_handsNode);
        if (!g_handsSkel) return false;
        void* buf = HeapAlloc(*g_pHeap, 0, 0x120);
        if (buf) {
            *(void**)((char*)g_handsSkel + 12) = buf;
            memset(buf, 0, 0x120);
        }
        *(DWORD*)((char*)g_handsSkel + 8) = 0;
        g_handsLoaded = true;
        FpvmLog("[FpvViewmodel] Hands loaded: node=0x%08X skel=0x%08X\n",
                (unsigned)(uintptr_t)g_handsNode, (unsigned)(uintptr_t)g_handsSkel);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        FpvmLog("[FpvViewmodel] CRASH loading hands\n");
        g_initialized = false;
        return false;
    }
}

// ============================================================================
// Hook on GAM_FPV_RenderWeapon — called from GAM_DoTick AFTER scene render.
// This is the correct pipeline stage for FPV overlay rendering.
// ============================================================================

static void __cdecl Hooked_RenderWeapon(int is_mirror_setup, int is_mirror_render) {
    // Don't call original (it early-exits on NULL g_localPlayer)
    // Only render on non-mirror call
    if (!g_wantRender || !g_handsLoaded || is_mirror_render)
        return;

    g_wantRender = false;

    // Swap g_localPlayer to spectated entity so HUD shows their data
    DWORD* savedLP = *g_ppLocalPlayer;
    *g_ppLocalPlayer = (DWORD*)g_spectatedEntity;

    __try {
        g_pfnIskeSkeletSet(g_handsSkel, *g_pDeltaTime);

        struct { void* node; void* skeleton; } renderBuf;
        renderBuf.node = g_handsNode;
        renderBuf.skeleton = g_handsSkel;
        g_pfnRenRender(&renderBuf, 1);

        static int s_ok = 0;
        if (s_ok < 5) {
            FpvmLog("[FpvViewmodel] Rendered frame %d\n", s_ok);
            FpvmLog("[FpvViewmodel] Rendered OK\n");
            s_ok++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_c = 0;
        if (s_c++ < 3) FpvmLog("[FpvViewmodel] Render crash\n");
    }

    // Restore
    *g_ppLocalPlayer = savedLP;
}

// ============================================================================
// Init
// ============================================================================

bool InitFpvViewmodel(uintptr_t gameBase) {
    g_gameBase = gameBase;
    g_pDeltaTime    = (float*)(gameBase + RVA_DELTA_TIME);
    g_pHeap         = (HANDLE*)(gameBase + RVA_HEAP);
    g_ppLocalPlayer = (DWORD**)(gameBase + RVA_LOCAL_PLAYER);

    g_pfnBesLoad       = *(BesLoad_t*)(gameBase + RVA_BES_LOAD_IAT);
    g_pfnAddScene      = *(XbesAddScene_t*)(gameBase + RVA_XBES_ADD_SCENE_IAT);
    g_pfnGetFirstIske  = *(BesGetFirstIske_t*)(gameBase + RVA_BES_FIRST_ISKE_IAT);
    g_pfnIskeSkeletSet = *(IskeSkeletSet_t*)(gameBase + RVA_ISKE_SKELET_SET_IAT);
    g_pfnRenRender     = *(RenRenderNodIske_t*)(gameBase + RVA_REN_RENDER_IAT);

    // Hook GAM_FPV_RenderWeapon — find by AOB
    BYTE* scan = (BYTE*)gameBase;
    void* renderAddr = nullptr;
    for (DWORD i = 0; i < 0x860000 - 18; i++) {
        // Pattern: A1 ?? ?? ?? ?? 81 EC C4 01 00 00 53 55 33 ED 3B C5 56
        if (scan[i]==0xA1 && scan[i+5]==0x81 && scan[i+6]==0xEC
            && scan[i+7]==0xC4 && scan[i+8]==0x01 && scan[i+9]==0x00 && scan[i+10]==0x00
            && scan[i+11]==0x53 && scan[i+12]==0x55 && scan[i+13]==0x33 && scan[i+14]==0xED) {
            renderAddr = &scan[i];
            break;
        }
    }
    if (!renderAddr) {
        FpvmLog("[FpvViewmodel] AOB scan for RenderWeapon FAILED\n");
        return false;
    }

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
    st = MH_CreateHook(renderAddr, (void*)&Hooked_RenderWeapon, (void**)&g_origRenderWeapon);
    if (st != MH_OK) { FpvmLog("[FpvViewmodel] Hook create failed: %d\n", st); return false; }
    MH_EnableHook(renderAddr);
    FpvmLog("[FpvViewmodel] Hooked RenderWeapon at 0x%08X\n", (unsigned)(uintptr_t)renderAddr);

    g_initialized = true;
    return true;
}

void ShutdownFpvViewmodel() {
    g_initialized = false;
    if (g_fpvmLog) { fclose(g_fpvmLog); g_fpvmLog = nullptr; }
}

void* FpvViewmodel_GetSkeleton() { return g_handsSkel; }

bool FpvViewmodel_RenderFrame(void* playerEntity) {
    if (!g_initialized || !playerEntity) return false;
    if (!g_handsLoaded && !LoadHands()) return false;
    g_spectatedEntity = playerEntity;
    g_wantRender = true;
    return true;
}
