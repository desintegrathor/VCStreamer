#define _CRT_SECURE_NO_WARNINGS
#include "TickDelayBuffer.h"
#include "DelayManager.h"
#include "RealtimeHook.h"
#include <iostream>
#include <fstream>
#include <string>
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
    vfprintf(g_tickLog, fmt, args);
    fflush(g_tickLog);
    va_end(args);
    // Also stdout
    va_start(args, fmt);
    vprintf(fmt, args);
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

static int g_tickDelayMs = 5000;  // Default 5 seconds, read from INI "tick_delay"

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
// INI loading
// ============================================================================

static int LoadTickDelayFromINI() {
    char dllPath[MAX_PATH];
    HMODULE hModule = NULL;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)&LoadTickDelayFromINI,
                          &hModule)) {
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);
        std::string fullPath(dllPath);
        size_t lastSlash = fullPath.find_last_of("\\/");
        std::string iniPath = fullPath.substr(0, lastSlash + 1) + "vcstreamer.ini";

        char buf[32];
        GetPrivateProfileStringA("vcstreamer", "tick_delay", "",
                                 buf, sizeof(buf), iniPath.c_str());
        if (buf[0] != '\0') {
            int val = atoi(buf);
            if (val > 0) return val * 1000;
        }

        // Also try flat key format (no section)
        std::ifstream iniFile(iniPath);
        if (iniFile.is_open()) {
            std::string line;
            while (std::getline(iniFile, line)) {
                if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                // Trim
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t\r\n"));
                value.erase(value.find_last_not_of(" \t\r\n") + 1);

                if (key == "tick_delay") {
                    int val = atoi(value.c_str());
                    if (val > 0) return val * 1000;
                }
            }
        }
    }

    return 5000;  // Default 5 seconds
}

// ============================================================================
// Public API
// ============================================================================

void InitTickDelayBuffer(uintptr_t gameBase) {
    g_gameBase = gameBase;

    // Load config
    g_tickDelayMs = LoadTickDelayFromINI();
    TickLog("[TickDelay] Packet delay: %dms (%ds)\n", g_tickDelayMs, g_tickDelayMs / 1000);

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
