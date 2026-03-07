#pragma once
#include <Windows.h>

// Initialize realtime kill detection.
// No longer patches game code — kill scanning is done from the TickDelayBuffer hook.
bool InitRealtimeHook(uintptr_t baseGame);

// Shutdown and cleanup.
void ShutdownRealtimeHook();

// Signal that all DLL initialization is complete and kill scanning can begin.
void SetHookReady();

// Scan raw network buffer for kill messages (opcode 0x50).
// Called from Hooked_NET_ReadMessages with real-time data before it enters the delay buffer.
void ScanBufferForKills(BYTE* data, DWORD size);

// Scan raw network buffer for flag carrier updates (opcode 0x5A, indices 0x200/0x201).
// Called from Hooked_NET_ReadMessages with real-time data before it enters the delay buffer.
void ScanBufferForFlags(BYTE* data, DWORD size);
