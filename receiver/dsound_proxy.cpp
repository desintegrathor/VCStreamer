#include "dsound_proxy.h"

// Function pointer table for all 12 real dsound exports
FARPROC g_dsound[12];
static HMODULE g_realDsound = NULL;

static void FillTable() {
    g_dsound[0]  = GetProcAddress(g_realDsound, "DirectSoundCreate");
    g_dsound[1]  = GetProcAddress(g_realDsound, "DirectSoundEnumerateA");
    g_dsound[2]  = GetProcAddress(g_realDsound, "DirectSoundEnumerateW");
    g_dsound[3]  = GetProcAddress(g_realDsound, "DllCanUnloadNow");
    g_dsound[4]  = GetProcAddress(g_realDsound, "DllGetClassObject");
    g_dsound[5]  = GetProcAddress(g_realDsound, "DirectSoundCaptureCreate");
    g_dsound[6]  = GetProcAddress(g_realDsound, "DirectSoundCaptureEnumerateA");
    g_dsound[7]  = GetProcAddress(g_realDsound, "DirectSoundCaptureEnumerateW");
    g_dsound[8]  = GetProcAddress(g_realDsound, "GetDeviceID");
    g_dsound[9]  = GetProcAddress(g_realDsound, "DirectSoundFullDuplexCreate");
    g_dsound[10] = GetProcAddress(g_realDsound, "DirectSoundCreate8");
    g_dsound[11] = GetProcAddress(g_realDsound, "DirectSoundCaptureCreate8");
}

static HMODULE GetOurself() {
    HMODULE ourself = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&g_dsound, &ourself);
    return ourself;
}

static volatile LONG g_initStarted = 0;
static volatile LONG g_ready = 0;

void LoadRealDsound() {
    // Called from DllMain — do nothing. Deferred to first stub call.
}

static void EnsureLoaded() {
    if (g_ready) return;

    if (InterlockedCompareExchange(&g_initStarted, 1, 0) == 0) {
        HMODULE ourself = GetOurself();
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        char path[MAX_PATH];
        wsprintfA(path, "%s\\dsound.dll", sysDir);

        // First try: maybe the real dsound is already loaded
        // Use the full path with GetModuleHandleA — won't find by full path,
        // so we need to enumerate or use LoadLibraryExA.
        g_realDsound = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

        if (!g_realDsound || g_realDsound == ourself) {
            g_realDsound = NULL;
        } else {
            FillTable();
        }
        InterlockedExchange(&g_ready, 1);
    } else {
        while (!g_ready) { Sleep(0); }
    }
}

static FARPROC __stdcall ResolveAndGet(int idx) {
    EnsureLoaded();
    return g_dsound[idx];
}

#define PROXY_STUB(name, idx) \
    __declspec(naked) void __stdcall proxy_##name() { \
        __asm push idx \
        __asm call ResolveAndGet \
        __asm test eax, eax \
        __asm jz _proxy_ret_##name \
        __asm jmp eax \
        __asm _proxy_ret_##name: \
        __asm ret \
    }

PROXY_STUB(DirectSoundCreate, 0)
PROXY_STUB(DirectSoundEnumerateA, 1)
PROXY_STUB(DirectSoundEnumerateW, 2)
PROXY_STUB(DllCanUnloadNow, 3)
PROXY_STUB(DllGetClassObject, 4)
PROXY_STUB(DirectSoundCaptureCreate, 5)
PROXY_STUB(DirectSoundCaptureEnumerateA, 6)
PROXY_STUB(DirectSoundCaptureEnumerateW, 7)
PROXY_STUB(GetDeviceID, 8)
PROXY_STUB(DirectSoundFullDuplexCreate, 9)
PROXY_STUB(DirectSoundCreate8, 10)
PROXY_STUB(DirectSoundCaptureCreate8, 11)
