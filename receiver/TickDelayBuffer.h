#pragma once
#include <Windows.h>

// Initialize the IAT hook on NET_ReadMessages for raw packet sniffing.
// Local packet replay buffering is disabled by default.
// Must be called after game.dll is loaded but before the game starts receiving packets.
void InitTickDelayBuffer(uintptr_t gameBase);

// Remove the IAT hook and free any optional replay buffer.
void ShutdownTickDelayBuffer();

// Returns the current local packet delay in milliseconds. Default is 0.
int GetTickDelayMs();
