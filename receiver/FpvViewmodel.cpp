#define _CRT_SECURE_NO_WARNINGS
#include "FpvViewmodel.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <windows.h>

// ============================================================================
// FPV Viewmodel — Path C stub
// Currently: loads hands mesh, positions skeleton.
// TODO: render hands via D3D8 hook (EndScene) with z-buffer clear.
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

static uintptr_t g_gameBase = 0;
static bool g_initialized = false;

bool InitFpvViewmodel(uintptr_t gameBase) {
    g_gameBase = gameBase;
    g_initialized = true;
    return true;
}

void ShutdownFpvViewmodel() {
    g_initialized = false;
    if (g_fpvmLog) { fclose(g_fpvmLog); g_fpvmLog = nullptr; }
}

void* FpvViewmodel_GetSkeleton() {
    return nullptr; // TODO: return our loaded skeleton
}

bool FpvViewmodel_RenderFrame(void* playerEntity) {
    // TODO: Path C rendering via D3D8 hook
    return false;
}
