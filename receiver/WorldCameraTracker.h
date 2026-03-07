#pragma once
#include <Windows.h>

// Initialize world camera tracker. Call after game.dll base is known.
void InitWorldCameraTracker(uintptr_t baseGame);

// Set target player to track with a world camera (thread-safe).
// playerHandle = DP handle of the player to follow (from kill/flag events).
void WorldCameraTracker_SetTarget(int playerHandle);

// Stop tracking — revert to normal spectator behavior.
void WorldCameraTracker_ClearTarget();

// Called every frame from Hooked_SpectInput (main thread).
// spectObj = pointer to spectator view object, baseGame = game.dll base.
void WorldCameraTracker_Update(int* spectObj, uintptr_t baseGame);
