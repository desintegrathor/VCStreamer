#pragma once
#include <Windows.h>

// Initialize auto-spectator polling thread.
// Monitors game state and automatically:
// 1. Waits for connection to server
// 2. Waits for game/map to load
// 3. Sets player mode to spectator
// 4. Switches camera to player-follow mode
// Must be called after game.dll is loaded.
void InitAutoSpectator(uintptr_t baseGame);

// Stop the auto-spectator thread.
void ShutdownAutoSpectator();
