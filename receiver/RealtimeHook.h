#pragma once
#include <Windows.h>

// Initialize inline hook on game.dll's message dispatch loop.
// This intercepts all network messages at the per-message dispatch point,
// giving us real-time access to kill events (type 0x22) which bypass the delay buffer.
// Must be called after game.dll is loaded.
// Returns true on success, false on failure.
bool InitRealtimeHook(uintptr_t baseGame);

// Remove the inline hook and restore original bytes.
void ShutdownRealtimeHook();

// Signal that all DLL initialization is complete and the hook can process messages.
// Must be called after all Init*() functions have completed.
void SetHookReady();
