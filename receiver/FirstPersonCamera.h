#pragma once
#include <Windows.h>
#include "SpectatedPlayerData.h"

// Initialize and install the first-person camera hook
// Must be called after game.dll is loaded
// Returns true on success, false on failure
bool InitFirstPersonCamera(uintptr_t gameBase);

// Cleanup and remove the hook
void ShutdownFirstPersonCamera();

// Get the current spectated player's combat state (updated each frame)
const SpectatedPlayerState& GetSpectatedPlayerState();
