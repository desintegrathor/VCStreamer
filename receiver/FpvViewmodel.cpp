#define _CRT_SECURE_NO_WARNINGS
#include "FpvViewmodel.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <iostream>
#include <windows.h>

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
// Game.dll IAT imports (RVA = IDA_IAT_addr - 0xD70000)
// ============================================================================

constexpr uintptr_t RVA_BES_LOAD_IAT        = 0x1E8010;
constexpr uintptr_t RVA_XBES_ADD_SCENE_IAT  = 0x1E7D3C;
constexpr uintptr_t RVA_BES_FIRST_ISKE_IAT  = 0x1E73E4;
constexpr uintptr_t RVA_ISKE_SKELET_SET_IAT = 0x1E7E08;
constexpr uintptr_t RVA_REN_RENDER_IAT      = 0x1E7B10;
constexpr uintptr_t RVA_DELTA_TIME          = 0x777714;
constexpr uintptr_t RVA_HEAP                = 0x77774C;

// ============================================================================
// Function types
// ============================================================================

typedef void*  (__cdecl *BesLoad_t)(const char* path, unsigned long flags, void* extra);
typedef unsigned long (__cdecl *XbesAddScene_t)(void* node);
typedef void*  (__cdecl *BesGetFirstIske_t)(void* node);
typedef void   (__cdecl *IskeSkeletSet_t)(void* skeleton, float deltaTime);
typedef void   (__cdecl *RenRenderNodIske_t)(void* renderBuf, unsigned int flags);

// ============================================================================
// State
// ============================================================================

static uintptr_t g_gameBase = 0;
static bool g_initialized = false;

static float*  g_pDeltaTime = nullptr;
static HANDLE* g_pHeap = nullptr;

static BesLoad_t          g_pfnBesLoad = nullptr;
static XbesAddScene_t     g_pfnAddScene = nullptr;
static BesGetFirstIske_t  g_pfnGetFirstIske = nullptr;
static IskeSkeletSet_t    g_pfnIskeSkeletSet = nullptr;
static RenRenderNodIske_t g_pfnRenRender = nullptr;

static void* g_handsNode = nullptr;
static void* g_handsSkel = nullptr;
static bool  g_handsLoaded = false;

// ============================================================================
// Init
// ============================================================================

bool InitFpvViewmodel(uintptr_t gameBase) {
    g_gameBase = gameBase;
    g_pDeltaTime = (float*)(gameBase + RVA_DELTA_TIME);
    g_pHeap      = (HANDLE*)(gameBase + RVA_HEAP);

    g_pfnBesLoad       = *(BesLoad_t*)(gameBase + RVA_BES_LOAD_IAT);
    g_pfnAddScene      = *(XbesAddScene_t*)(gameBase + RVA_XBES_ADD_SCENE_IAT);
    g_pfnGetFirstIske  = *(BesGetFirstIske_t*)(gameBase + RVA_BES_FIRST_ISKE_IAT);
    g_pfnIskeSkeletSet = *(IskeSkeletSet_t*)(gameBase + RVA_ISKE_SKELET_SET_IAT);
    g_pfnRenRender     = *(RenRenderNodIske_t*)(gameBase + RVA_REN_RENDER_IAT);

    g_initialized = true;
    printf("[FpvViewmodel] Initialized, base=0x%08X\n", (unsigned int)gameBase);
    fflush(stdout);
    FpvmLog("[FpvViewmodel] Initialized\n");
    return true;
}

void ShutdownFpvViewmodel() {
    g_initialized = false;
    if (g_fpvmLog) { fclose(g_fpvmLog); g_fpvmLog = nullptr; }
}

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

        // Allocate animation layer buffer at skeleton+12
        void* buf = HeapAlloc(*g_pHeap, 0, 0x120);
        if (buf) {
            *(void**)((char*)g_handsSkel + 12) = buf;
            memset(buf, 0, 0x120);
        }
        *(DWORD*)((char*)g_handsSkel + 8) = 0; // 0 layers

        g_handsLoaded = true;
        std::cout << "[FpvViewmodel] Hands loaded: node=0x" << std::hex
                  << (uintptr_t)g_handsNode << " skel=0x" << (uintptr_t)g_handsSkel
                  << std::dec << "\n";
        FpvmLog("[FpvViewmodel] Hands loaded OK\n");
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::cout << "[FpvViewmodel] CRASH loading hands\n";
        g_initialized = false;
        return false;
    }
}

// ============================================================================
// Public API
// ============================================================================

void* FpvViewmodel_GetSkeleton() {
    return g_handsSkel;
}

bool FpvViewmodel_RenderFrame(void* playerEntity) {
    static int s_callLog = 0;
    if (s_callLog < 3) {
        FpvmLog("[FpvViewmodel] RenderFrame called: init=%d entity=0x%08X loaded=%d\n",
            g_initialized, (uintptr_t)playerEntity, g_handsLoaded);
        s_callLog++;
    }
    if (!g_initialized || !playerEntity) return false;
    if (!g_handsLoaded && !LoadHands()) return false;

    __try {
        g_pfnIskeSkeletSet(g_handsSkel, *g_pDeltaTime);

        struct { void* node; void* skeleton; } renderBuf;
        renderBuf.node = g_handsNode;
        renderBuf.skeleton = g_handsSkel;
        g_pfnRenRender(&renderBuf, 1);

        static int s_ok = 0;
        if (s_ok < 3) {
            std::cout << "[FpvViewmodel] RENDERED frame " << s_ok << "\n";
            FpvmLog("[FpvViewmodel] Rendered OK\n");
            s_ok++;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_c = 0;
        if (s_c++ < 5) {
            std::cout << "[FpvViewmodel] RENDER CRASH #" << s_c << "\n";
            FpvmLog("[FpvViewmodel] Render crash #%d\n", s_c);
        }
        if (s_c >= 5) g_initialized = false;
        return false;
    }
}
