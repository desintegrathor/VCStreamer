#define _CRT_SECURE_NO_WARNINGS
#include "OctCollision.h"
#include "OctParser.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include "AutoSpectator.h"

// Game memory offsets (RVA from game.dll base)
constexpr uintptr_t LEVEL_DIR_OFFSET  = 0x778474;  // IDA 0x14E8474 — e.g. "Levels\NVA_BASE"
constexpr uintptr_t LEVEL_NAME_OFFSET = 0x6AA2D0;  // IDA 0x141A2D0 — e.g. "NVA_BASE"

// BF_ function typedefs (logs.dll exports)
typedef int   (__cdecl *BF_FileExist_t)(const char* path);
typedef void* (__cdecl *BF_FileOpen_t)(const char* path, unsigned long flags);
typedef unsigned long (__cdecl *BF_FileRead_t)(void* handle, void* buf, unsigned long size);
typedef unsigned __int64 (__cdecl *BF_FileSize_t)(void* handle);
typedef void  (__cdecl *BF_FileClose_t)(void* handle);

static BF_FileExist_t  g_BF_FileExist  = nullptr;
static BF_FileOpen_t   g_BF_FileOpen   = nullptr;
static BF_FileRead_t   g_BF_FileRead   = nullptr;
static BF_FileSize_t   g_BF_FileSize   = nullptr;
static BF_FileClose_t  g_BF_FileClose  = nullptr;

static uintptr_t g_baseGame = 0;
static OctTree   g_octTree;
static bool      g_loaded = false;
static char      g_loadedPath[512] = {};

extern void LogDebug(const char* fmt, ...);

static bool ResolveBF()
{
    HMODULE hLogs = GetModuleHandleA("logs.dll");
    if (!hLogs) {
        LogDebug("[OctCollision] logs.dll not loaded\n");
        return false;
    }

    g_BF_FileExist = (BF_FileExist_t)GetProcAddress(hLogs, "?BF_FileExist@@YAHPAD@Z");
    g_BF_FileOpen  = (BF_FileOpen_t) GetProcAddress(hLogs, "?BF_FileOpen@@YAPAXPADK@Z");
    g_BF_FileRead  = (BF_FileRead_t) GetProcAddress(hLogs, "?BF_FileRead@@YAKPAX0K@Z");
    g_BF_FileSize  = (BF_FileSize_t) GetProcAddress(hLogs, "?BF_FileSize@@YA_KPAX@Z");
    g_BF_FileClose = (BF_FileClose_t)GetProcAddress(hLogs, "?BF_FileClose@@YAXPAX@Z");

    if (!g_BF_FileExist || !g_BF_FileOpen || !g_BF_FileRead || !g_BF_FileSize || !g_BF_FileClose) {
        LogDebug("[OctCollision] Failed to resolve BF_ functions: Exist=%p Open=%p Read=%p Size=%p Close=%p\n",
                 g_BF_FileExist, g_BF_FileOpen, g_BF_FileRead, g_BF_FileSize, g_BF_FileClose);
        return false;
    }

    LogDebug("[OctCollision] BF_ functions resolved OK\n");
    return true;
}

void OctCollision_Init(uintptr_t baseGame)
{
    g_baseGame = baseGame;
    g_loaded = false;
    g_loadedPath[0] = '\0';

    if (!ResolveBF()) {
        LogDebug("[OctCollision] Init failed — BF_ functions not available\n");
    } else {
        LogDebug("[OctCollision] Init OK, base=0x%08X\n", baseGame);
    }
}

bool OctCollision_LoadCurrentMap()
{
    if (!g_BF_FileOpen) {
        // Try resolving again (logs.dll may have loaded later)
        if (!ResolveBF()) return false;
    }

    const char* levelDir  = (const char*)(g_baseGame + LEVEL_DIR_OFFSET);
    const char* levelName = (const char*)(g_baseGame + LEVEL_NAME_OFFSET);

    if (levelDir[0] == '\0' || levelName[0] == '\0') {
        LogDebug("[OctCollision] Level dir/name empty (dir='%s', name='%s')\n", levelDir, levelName);
        return false;
    }

    // Build VFS path: "Levels\MAP\data\MAP\col.oct"
    char octPath[512];
    _snprintf(octPath, sizeof(octPath), "%s\\data\\%s\\col.oct", levelDir, levelName);
    octPath[sizeof(octPath) - 1] = '\0';

    // Skip if already loaded for this path
    if (g_loaded && strcmp(g_loadedPath, octPath) == 0) {
        LogDebug("[OctCollision] Already loaded: %s\n", octPath);
        return true;
    }

    LogDebug("[OctCollision] Loading: %s\n", octPath);

    // Check existence
    if (!g_BF_FileExist(octPath)) {
        // Try _MP suffix fallback
        _snprintf(octPath, sizeof(octPath), "%s\\data\\%s_MP\\col.oct", levelDir, levelName);
        octPath[sizeof(octPath) - 1] = '\0';
        LogDebug("[OctCollision] Primary not found, trying: %s\n", octPath);

        if (!g_BF_FileExist(octPath)) {
            LogDebug("[OctCollision] col.oct not found in VFS\n");
            g_loaded = false;
            return false;
        }
    }

    void* handle = g_BF_FileOpen(octPath, 0);
    if (!handle) {
        LogDebug("[OctCollision] BF_FileOpen failed for: %s\n", octPath);
        g_loaded = false;
        return false;
    }

    unsigned __int64 fileSize64 = g_BF_FileSize(handle);
    size_t fileSize = (size_t)fileSize64;

    if (fileSize < 80) {
        LogDebug("[OctCollision] File too small: %u bytes\n", (unsigned)fileSize);
        g_BF_FileClose(handle);
        g_loaded = false;
        return false;
    }

    uint8_t* buf = new (std::nothrow) uint8_t[fileSize];
    if (!buf) {
        LogDebug("[OctCollision] Alloc failed for %u bytes\n", (unsigned)fileSize);
        g_BF_FileClose(handle);
        g_loaded = false;
        return false;
    }

    unsigned long bytesRead = g_BF_FileRead(handle, buf, (unsigned long)fileSize);
    g_BF_FileClose(handle);

    if (bytesRead != (unsigned long)fileSize) {
        LogDebug("[OctCollision] BF_FileRead: expected %u, got %u\n", (unsigned)fileSize, bytesRead);
        delete[] buf;
        g_loaded = false;
        return false;
    }

    OctTree newTree;
    bool ok = OctParseFromMemory(buf, fileSize, newTree);
    delete[] buf;

    if (!ok) {
        LogDebug("[OctCollision] OctParseFromMemory failed\n");
        g_loaded = false;
        return false;
    }

    g_octTree = std::move(newTree);
    g_loaded = true;
    strncpy(g_loadedPath, octPath, sizeof(g_loadedPath) - 1);
    g_loadedPath[sizeof(g_loadedPath) - 1] = '\0';

    LogDebug("[OctCollision] Loaded OK: %u triangles, %u nodes, center=(%.1f,%.1f,%.1f), radius=%.1f\n",
             (unsigned)g_octTree.triangles.size(), (unsigned)g_octTree.nodes.size(),
             g_octTree.nodes.empty() ? 0.f : g_octTree.nodes[0].center.x,
             g_octTree.nodes.empty() ? 0.f : g_octTree.nodes[0].center.y,
             g_octTree.nodes.empty() ? 0.f : g_octTree.nodes[0].center.z,
             g_octTree.nodes.empty() ? 0.f : g_octTree.nodes[0].radius);
    return true;
}

float OctCollision_Raycast(const float* origin, const float* dir, float* hitNormal)
{
    if (!g_loaded) return FLT_MAX;

    OctVec3 o = { origin[0], origin[1], origin[2] };
    OctVec3 d = { dir[0], dir[1], dir[2] };
    OctVec3 normal;

    float t = OctRaycast(g_octTree, o, d, hitNormal ? &normal : nullptr);

    if (hitNormal && t < FLT_MAX) {
        hitNormal[0] = normal.x;
        hitNormal[1] = normal.y;
        hitNormal[2] = normal.z;
    }
    return t;
}

bool OctCollision_LineOfSight(const float* from, const float* to)
{
    if (!g_loaded) return true; // no data = assume visible

    OctVec3 f = { from[0], from[1], from[2] };
    OctVec3 t = { to[0], to[1], to[2] };
    return OctLineOfSight(g_octTree, f, t);
}

bool OctCollision_IsLoaded()
{
    return g_loaded;
}
