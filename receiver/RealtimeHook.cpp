#include "RealtimeHook.h"
#include "SpectatorController.h"
#include "DelayManager.h"
#include <iostream>
#include <cstring>
#include <mutex>
#include <unordered_set>

// ============================================================================
// Offsets from game.dll base
// ============================================================================

// Message dispatch loop body - executed for EVERY incoming message before
// the switch statement routes it. This catches ALL message types including
// type 0x22 (kill) which never reaches the delay buffer enqueue.
// At this point: ebx = pointer to message data (first byte = message type)
constexpr uintptr_t OFFSET_MSG_DISPATCH = 0x15CD2E;

// ============================================================================
// Message type constants (first byte of message data)
// ============================================================================

constexpr BYTE MSG_KILL = 0x22;       // Kill event (34 decimal)

// ============================================================================
// Kill deduplication
// ============================================================================

// Key: simple hash of killer + victim + approximate time
// Used to avoid processing the same kill twice (once at enqueue, once at dequeue)
static std::mutex g_realtimeMutex;
static std::unordered_set<uint64_t> g_recentKills;
static DWORD g_lastKillCleanup = 0;
constexpr DWORD KILL_DEDUP_WINDOW_MS = 30000; // 30s window for dedup
constexpr DWORD KILL_CLEANUP_INTERVAL_MS = 10000; // Cleanup every 10s

static uint64_t MakeKillKey(int killerId, int victimId) {
    // Combine killer and victim IDs with a coarse time bucket (10s granularity)
    DWORD timeBucket = GetTickCount() / 10000;
    return ((uint64_t)killerId << 32) | ((uint64_t)victimId << 16) | (timeBucket & 0xFFFF);
}

// ============================================================================
// Hook globals
// ============================================================================

static uintptr_t g_baseGame = 0;
static uintptr_t g_hookAddr = 0;
static uintptr_t g_returnAddr = 0;
static BYTE g_originalBytes[9];
static bool g_hookInstalled = false;

// Guard flag: only process messages after DLL is fully initialized
static volatile bool g_hookReady = false;

// VirtualAlloc'd code cave for our trampoline
static BYTE* g_codeCave = nullptr;

// ============================================================================
// Message handler - called from hook with real-time message data
// ============================================================================

static void __cdecl OnMessageBeforeDelay(BYTE* msgData) {
    if (!msgData) return;

    BYTE subType = msgData[0];

    // ---------------------------
    // KILL EVENT (0x22)
    // ---------------------------
    if (subType == MSG_KILL) {
        // Kill message format:
        //   Byte 0:    0x22 (message type)
        //   Byte 1:    weapon/cause ID
        //   Bytes 2-5: victim player handle (DWORD)
        //   Bytes 6-9: killer player handle (DWORD)
        int victimHandle = *(int*)(msgData + 2);
        int killerHandle = *(int*)(msgData + 6);

        // Skip invalid handles
        if (killerHandle == 0 && victimHandle == 0) return;

        // Deduplication: check if we already processed this kill
        uint64_t key = MakeKillKey(killerHandle, victimHandle);
        {
            std::lock_guard<std::mutex> lock(g_realtimeMutex);

            // Periodic cleanup of old entries
            DWORD now = GetTickCount();
            if (now - g_lastKillCleanup > KILL_CLEANUP_INTERVAL_MS) {
                g_recentKills.clear();
                g_lastKillCleanup = now;
            }

            // Check and insert
            if (g_recentKills.count(key)) {
                return; // Already processed
            }
            g_recentKills.insert(key);
        }

        // Player handles in kill messages ARE the player IDs (offset 0 of player struct)

        // Read the spectator delay from game memory (set by server)
        int gameDelaySec = DelayManager::GetGameDelaySeconds();

        // We want camera to switch 5s BEFORE the spectator view shows the kill.
        // Hook fires at realtime (0s), spectator shows at +gameDelay seconds.
        // So we schedule: (gameDelay - 5) seconds from now.
        // If delay <= 5s, execute immediately.
        constexpr int ADVANCE_SECONDS = 5;
        int waitMs = (gameDelaySec > ADVANCE_SECONDS)
            ? (gameDelaySec - ADVANCE_SECONDS) * 1000
            : 0;

        std::cout << "[RealtimeHook] Kill detected: killer=" << killerHandle
                  << " victim=" << victimHandle
                  << " weapon=" << (int)msgData[1]
                  << " gameDelay=" << gameDelaySec << "s"
                  << " scheduling in " << waitMs << "ms\n";

        auto action = DelayedAction::CreateKillAction(killerHandle, victimHandle, waitMs);
        DelayManager::AddDelayedAction(action);
    }
}

// ============================================================================
// Naked hook trampoline (x86 inline assembly)
// ============================================================================

// The hook replaces 9 bytes at game.dll + 0x15CD2E (message dispatch loop body):
//
// Original:
//   ECCD2E: 33 C0           xor eax, eax
//   ECCD30: 8A 03           mov al, [ebx]         ; ebx = msg data ptr, al = type byte
//   ECCD32: 05 30 1B 0F 00  add eax, 0F1B30h
//
// We overwrite with: JMP to code cave (5 bytes) + 4x NOP
// Code cave does: save regs, push ebx, call handler, cleanup, restore regs,
//                 execute original 3 instructions, JMP back to ECCD37 (push eax)
//
// Register state at hook point:
//   ebx = pointer to current message data (first byte = message type)

// ============================================================================
// Code cave builder
// ============================================================================

static bool BuildCodeCave() {
    // Allocate executable memory for our code cave
    g_codeCave = (BYTE*)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_codeCave) {
        std::cout << "[RealtimeHook] Failed to allocate code cave\n";
        return false;
    }

    int i = 0;

    // --- Save all state ---
    g_codeCave[i++] = 0x60;  // pushad
    g_codeCave[i++] = 0x9C;  // pushfd

    // --- Guard flag check: cmp byte ptr [&g_hookReady], 0 ---
    g_codeCave[i++] = 0x80;  // cmp byte ptr [imm32], imm8
    g_codeCave[i++] = 0x3D;
    uintptr_t guardAddr = (uintptr_t)&g_hookReady;
    memcpy(g_codeCave + i, &guardAddr, 4); i += 4;
    g_codeCave[i++] = 0x00;  // compare with 0

    // je skip_handler (patch offset later)
    g_codeCave[i++] = 0x74;  // je rel8
    int skipGuardPatchPos = i;
    g_codeCave[i++] = 0x00;  // placeholder

    // --- Read message type in assembly: xor eax, eax; mov al, [ebx] ---
    g_codeCave[i++] = 0x33; g_codeCave[i++] = 0xC0;  // xor eax, eax
    g_codeCave[i++] = 0x8A; g_codeCave[i++] = 0x03;  // mov al, [ebx]

    // --- Filter: cmp al, 0x22 (kill message type) ---
    g_codeCave[i++] = 0x3C; g_codeCave[i++] = 0x22;  // cmp al, 0x22

    // jne skip_handler (patch offset later)
    g_codeCave[i++] = 0x75;  // jne rel8
    int skipTypePatchPos = i;
    g_codeCave[i++] = 0x00;  // placeholder

    // --- Kill message detected: call C++ handler ---
    // push ebx (msgData pointer)
    g_codeCave[i++] = 0x53;

    // call OnMessageBeforeDelay
    uintptr_t callTarget = (uintptr_t)&OnMessageBeforeDelay;
    uintptr_t callFrom = (uintptr_t)(g_codeCave + i + 5);
    int32_t callRel = (int32_t)(callTarget - callFrom);
    g_codeCave[i++] = 0xE8;  // call rel32
    memcpy(g_codeCave + i, &callRel, 4); i += 4;

    // add esp, 4 (cleanup 1 arg)
    g_codeCave[i++] = 0x83; g_codeCave[i++] = 0xC4; g_codeCave[i++] = 0x04;

    // --- skip_handler label: both je's jump here ---
    int skipTarget = i;
    g_codeCave[skipGuardPatchPos] = (BYTE)(skipTarget - (skipGuardPatchPos + 1));
    g_codeCave[skipTypePatchPos] = (BYTE)(skipTarget - (skipTypePatchPos + 1));

    // --- Restore all state ---
    g_codeCave[i++] = 0x9D;  // popfd
    g_codeCave[i++] = 0x61;  // popad

    // --- Execute original 3 instructions that we overwrote (9 bytes) ---
    // 1. xor eax, eax  (33 C0)
    g_codeCave[i++] = 0x33; g_codeCave[i++] = 0xC0;
    // 2. mov al, [ebx]  (8A 03)
    g_codeCave[i++] = 0x8A; g_codeCave[i++] = 0x03;
    // 3. add eax, 0F1B30h  (05 30 1B 0F 00)
    g_codeCave[i++] = 0x05;
    g_codeCave[i++] = 0x30; g_codeCave[i++] = 0x1B;
    g_codeCave[i++] = 0x0F; g_codeCave[i++] = 0x00;

    // --- jmp back to original code (g_returnAddr = ECCD37) ---
    uintptr_t jmpFrom = (uintptr_t)(g_codeCave + i + 5);
    int32_t jmpRel = (int32_t)(g_returnAddr - jmpFrom);
    g_codeCave[i++] = 0xE9;  // jmp rel32
    memcpy(g_codeCave + i, &jmpRel, 4); i += 4;

    return true;
}

// ============================================================================
// Public API
// ============================================================================

void SetHookReady() {
    g_hookReady = true;
    std::cout << "[RealtimeHook] Hook activated (ready for messages)\n";
}

bool InitRealtimeHook(uintptr_t baseGame) {
    g_baseGame = baseGame;
    g_hookAddr = baseGame + OFFSET_MSG_DISPATCH;
    g_returnAddr = g_hookAddr + 9; // Skip 9 bytes (2 + 2 + 5)

    std::cout << "[RealtimeHook] Initializing realtime message hook...\n";
    std::cout << "[RealtimeHook] game.dll base: 0x" << std::hex << baseGame << "\n";
    std::cout << "[RealtimeHook] Hook address: 0x" << g_hookAddr << "\n";
    std::cout << "[RealtimeHook] Return address: 0x" << g_returnAddr << std::dec << "\n";

    // Save original bytes (9 bytes: xor eax,eax + mov al,[ebx] + add eax,0F1B30h)
    memcpy(g_originalBytes, (void*)g_hookAddr, 9);

    // Build the code cave
    if (!BuildCodeCave()) {
        return false;
    }

    // Patch the original code: JMP to code cave (5 bytes) + 4x NOP
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)g_hookAddr, 9, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::cout << "[RealtimeHook] Failed to change memory protection\n";
        VirtualFree(g_codeCave, 0, MEM_RELEASE);
        g_codeCave = nullptr;
        return false;
    }

    // Write JMP rel32 to code cave
    BYTE* hookPtr = (BYTE*)g_hookAddr;
    int32_t jmpOffset = (int32_t)((uintptr_t)g_codeCave - (g_hookAddr + 5));
    hookPtr[0] = 0xE9; // JMP rel32
    memcpy(hookPtr + 1, &jmpOffset, 4);

    // Fill remaining 4 bytes with NOPs
    hookPtr[5] = 0x90;
    hookPtr[6] = 0x90;
    hookPtr[7] = 0x90;
    hookPtr[8] = 0x90;

    // Restore original protection
    VirtualProtect((LPVOID)g_hookAddr, 9, oldProtect, &oldProtect);

    // Flush instruction cache
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)g_hookAddr, 9);

    g_hookInstalled = true;
    std::cout << "[RealtimeHook] Hook installed successfully!\n";
    return true;
}

void ShutdownRealtimeHook() {
    if (!g_hookInstalled) return;

    // Restore original bytes
    DWORD oldProtect;
    if (VirtualProtect((LPVOID)g_hookAddr, 9, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy((void*)g_hookAddr, g_originalBytes, 9);
        VirtualProtect((LPVOID)g_hookAddr, 9, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPVOID)g_hookAddr, 9);
    }

    // Free code cave
    if (g_codeCave) {
        VirtualFree(g_codeCave, 0, MEM_RELEASE);
        g_codeCave = nullptr;
    }

    g_hookInstalled = false;
    std::cout << "[RealtimeHook] Hook removed\n";
}
