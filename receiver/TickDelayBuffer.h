#pragma once
#include <Windows.h>

// Initialize the IAT hook on NET_ReadMessages to buffer and delay all incoming packets.
// Must be called after game.dll is loaded but before the game starts receiving packets.
void InitTickDelayBuffer(uintptr_t gameBase);

// Remove the IAT hook and free the ring buffer.
void ShutdownTickDelayBuffer();

// Returns the current tick delay in milliseconds (e.g. 5000).
int GetTickDelayMs();
