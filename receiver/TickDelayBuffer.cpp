#define _CRT_SECURE_NO_WARNINGS
#include "TickDelayBuffer.h"
#include "DelayManager.h"
#include "DiagnosticsLog.h"
#include "RealtimeHook.h"
#include <cstring>
#include <mutex>
#include <cstdarg>
#include <cstdio>

// File-based logging (visible after game exit)
static FILE* g_tickLog = nullptr;

static void TickLog(const char* fmt, ...) {
    if (!g_tickLog) {
        g_tickLog = fopen("tickdelay_debug.log", "w");
        if (!g_tickLog) return;
    }
    va_list args;
    va_start(args, fmt);
    DiagnosticsLog_Write(g_tickLog, fmt, args);
    va_end(args);
}

// ============================================================================
// NET_ReadMessages IAT hook for packet-level delay
// ============================================================================
//
// NET_ReadMessages is imported by game.dll from logs.dll.
// IAT entry at game.dll + 0x28794C.
//
// Signature:
//   unsigned int __cdecl NET_ReadMessages(
//       void* dpConnection,    // DirectPlay connection handle
//       void** outData,        // sender handle (output)
//       void** outData2,       // message type (output)
//       void* buffer,          // 60KB static buffer at game.dll+0x84E28
//       unsigned long* outSize // bytes written to buffer (output)
//   );
//   Returns 0 = success (got data), 0x0A = disconnect, other = no data
//

// ============================================================================
// Configuration
// ============================================================================

static const int g_tickDelayMs = 5000;  // Fixed 5-second safety buffer

// ============================================================================
// Original function pointer
// ============================================================================

typedef unsigned int(__cdecl* NET_ReadMessages_t)(
    void* dpConnection,
    void** outData,
    void** outData2,
    void* buffer,
    unsigned long* outSize
);

static NET_ReadMessages_t g_originalReadMessages = nullptr;
static uintptr_t g_iatAddr = 0;
static uintptr_t g_gameBase = 0;
static bool g_hookInstalled = false;

// g_playerArray (64 entity pointers) — RVA 0x7AE9C8 in game.dll
static void** g_playerArray = nullptr;
// g_localPlayer pointer-to-pointer — RVA 0x80D458
static void*** g_ppLocalPlayer = nullptr;

// Base game bug: GNET_CLN_PL_UpdateStatus writes decoded pitch to the OUTER
// player struct at +0x1E4 (shadow state). The renderer (GAM_PL_RenderSkeleton)
// and sender (GNET_PL_BuildMoveState) both read/write pitch on the INNER
// struct at +0xF8, reached via outer[61] (+0xF4) which holds a pointer to it.
// Nothing copies shadow (outer+0x1E4) -> applied (inner+0xF8) for remote
// players, so their weapons stay horizontal. Mirror it ourselves.
static void MirrorRemotePlayerPitch() {
    if (!g_playerArray || !g_ppLocalPlayer) return;
    void* localPlayer = *g_ppLocalPlayer;
    for (int i = 0; i < 64; ++i) {
        char* outer = (char*)g_playerArray[i];
        if (!outer || outer == localPlayer) continue;
        void* inner = *(void**)(outer + 0xF4);
        if (!inner) continue;
        *(float*)((char*)inner + 0xF8) = *(float*)(outer + 0x1E4);
    }
}

// ============================================================================
// Ring buffer for delayed packets
// ============================================================================

static const int MAX_BUFFER_SIZE = 65536;  // 60KB+ to match game's static buffer

struct BufferedChunk {
    DWORD timestamp;           // GetTickCount() when received
    DWORD dataSize;            // bytes of message data
    void* senderHandle;        // outData from NET_ReadMessages
    void* messageType;         // outData2
    BYTE data[MAX_BUFFER_SIZE];
};

static const int RING_SIZE = 1200;  // ~20s at 60fps
static BufferedChunk* g_ring = nullptr;
static int g_ringHead = 0;   // next write position
static int g_ringTail = 0;   // next read position
static int g_ringCount = 0;  // number of buffered chunks
static std::mutex g_ringMutex;

// Diagnostic logging
static bool g_firstPacketLogged = false;
static DWORD g_lastStatsLog = 0;

// ============================================================================
// Hooked NET_ReadMessages
// ============================================================================

static unsigned int __cdecl Hooked_NET_ReadMessages(
    void* dpConnection,
    void** outData,
    void** outData2,
    void* buffer,
    unsigned long* outSize
) {
    // Apply remote-player pitch fix every tick (game.dll never does this itself).
    MirrorRemotePlayerPitch();

    // 1. Always drain the real DirectPlay queue
    unsigned int result = g_originalReadMessages(dpConnection, outData, outData2, buffer, outSize);

    if (result == 0 && outSize && *outSize > 0) {
        // Got real data from network
        BYTE* rawBuf = (BYTE*)buffer;
        DWORD rawSize = *outSize;
        void* senderHandle = outData ? *outData : nullptr;
        void* messageType = outData2 ? *outData2 : nullptr;

        // Scan raw buffer for kill/flag messages before any splitting
        ScanBufferForKills(rawBuf, rawSize);
        ScanBufferForFlags(rawBuf, rawSize);
        ScanBufferForPlayerPositions(rawBuf, rawSize);

        // Pass through ALL messages until spectator has fully spawned
        // mpSubState must reach 10 (spectator spawned) before we start buffering
        // Otherwise join/spawn confirmations get delayed and mpSubState stays at 9
        if (g_gameBase) {
            constexpr uintptr_t MP_SUBSTATE_OFFSET = 0x7C0F58;
            int mpSubState = *(int*)(g_gameBase + MP_SUBSTATE_OFFSET);
            if (mpSubState != 10) {
                return 0;  // Pass through immediately
            }
        }

        // Log first intercepted packet
        if (!g_firstPacketLogged) {
            TickLog("[TickDelay] First packet intercepted! size=%lu\n", rawSize);
            g_firstPacketLogged = true;
        }

        // 2. Buffer the entire message with timestamp for delayed replay
        {
            std::lock_guard<std::mutex> lock(g_ringMutex);
            if (g_ringCount < RING_SIZE) {
                BufferedChunk& chunk = g_ring[g_ringHead];
                chunk.timestamp = GetTickCount();
                chunk.dataSize = rawSize;
                chunk.senderHandle = senderHandle;
                chunk.messageType = messageType;
                memcpy(chunk.data, rawBuf, rawSize);
                g_ringHead = (g_ringHead + 1) % RING_SIZE;
                g_ringCount++;
            } else {
                TickLog("[TickDelay] WARNING: Ring buffer full, dropping packet!\n");
            }
        }

        // 3. Check if we have delayed data ready to replay
        {
            std::lock_guard<std::mutex> lock(g_ringMutex);
            DWORD now = GetTickCount();

            if (g_ringCount > 0) {
                BufferedChunk& oldest = g_ring[g_ringTail];
                DWORD age = now - oldest.timestamp;

                if (age >= (DWORD)g_tickDelayMs) {
                    memcpy(buffer, oldest.data, oldest.dataSize);
                    *outSize = oldest.dataSize;
                    if (outData) *outData = oldest.senderHandle;
                    if (outData2) *outData2 = oldest.messageType;

                    g_ringTail = (g_ringTail + 1) % RING_SIZE;
                    g_ringCount--;

                    return 0;  // Success — we have data
                }
            }
        }

        // Periodic stats logging
        {
            DWORD now = GetTickCount();
            if (now - g_lastStatsLog > 10000) {
                TickLog("[TickDelay] Buffer: %d/%d delay=%dms\n", g_ringCount, RING_SIZE, g_tickDelayMs);
                g_lastStatsLog = now;
            }
        }

        // 4. We consumed real data but nothing to return yet (buffering phase)
        *outSize = 0;
        return 1;  // No data available
    }

    if (result == 0x0A) {
        // Disconnect — pass through immediately
        return result;
    }

    // No new data from network. Check if we have delayed data to replay.
    {
        std::lock_guard<std::mutex> lock(g_ringMutex);
        DWORD now = GetTickCount();

        if (g_ringCount > 0) {
            BufferedChunk& oldest = g_ring[g_ringTail];
            DWORD age = now - oldest.timestamp;

            if (age >= (DWORD)g_tickDelayMs) {
                memcpy(buffer, oldest.data, oldest.dataSize);
                *outSize = oldest.dataSize;
                if (outData) *outData = oldest.senderHandle;
                if (outData2) *outData2 = oldest.messageType;

                g_ringTail = (g_ringTail + 1) % RING_SIZE;
                g_ringCount--;

                return 0;  // Success
            }
        }
    }

    return result;  // No data
}

// ============================================================================
// Public API
// ============================================================================

void InitTickDelayBuffer(uintptr_t gameBase) {
    g_gameBase = gameBase;

    // Pointers used by MirrorRemotePlayerPitch — RVAs from game.dll v16.
    g_playerArray   = (void**)(gameBase + 0x7AE9C8);
    g_ppLocalPlayer = (void***)(gameBase + 0x80D458);

    TickLog("[TickDelay] Fixed buffer delay: %dms (%ds)\n", g_tickDelayMs, g_tickDelayMs / 1000);

    // Allocate ring buffer
    g_ring = new BufferedChunk[RING_SIZE];
    memset(g_ring, 0, sizeof(BufferedChunk) * RING_SIZE);
    g_ringHead = 0;
    g_ringTail = 0;
    g_ringCount = 0;

    // Find IAT entry for NET_ReadMessages in game.dll
    // IAT offset: 0x1E794C (IDA addr 0xF5794C - base 0xD70000)
    constexpr uintptr_t IAT_OFFSET = 0x1E794C;
    g_iatAddr = gameBase + IAT_OFFSET;

    // Read the current function pointer from IAT
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)g_iatAddr, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        TickLog("[TickDelay] ERROR: Failed to unprotect IAT entry at 0x%08X\n", (unsigned)g_iatAddr);
        delete[] g_ring;
        g_ring = nullptr;
        return;
    }

    g_originalReadMessages = *(NET_ReadMessages_t*)g_iatAddr;
    TickLog("[TickDelay] Original NET_ReadMessages at: 0x%08X\n", (unsigned)(uintptr_t)g_originalReadMessages);

    // Patch IAT to point to our hook
    *(uintptr_t*)g_iatAddr = (uintptr_t)&Hooked_NET_ReadMessages;

    VirtualProtect((LPVOID)g_iatAddr, sizeof(void*), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)g_iatAddr, sizeof(void*));

    g_hookInstalled = true;
    TickLog("[TickDelay] IAT hook installed successfully!\n");
    TickLog("[TickDelay] Ring buffer: %d slots allocated\n", RING_SIZE);
}

void ShutdownTickDelayBuffer() {
    if (!g_hookInstalled) return;

    // Restore original IAT entry
    DWORD oldProtect;
    if (VirtualProtect((LPVOID)g_iatAddr, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        *(uintptr_t*)g_iatAddr = (uintptr_t)g_originalReadMessages;
        VirtualProtect((LPVOID)g_iatAddr, sizeof(void*), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPVOID)g_iatAddr, sizeof(void*));
    }

    // Free ring buffer
    if (g_ring) {
        delete[] g_ring;
        g_ring = nullptr;
    }

    g_hookInstalled = false;
    TickLog("[TickDelay] IAT hook removed, buffer freed\n");
    if (g_tickLog) { fclose(g_tickLog); g_tickLog = nullptr; }
}

int GetTickDelayMs() {
    return g_tickDelayMs;
}
