#pragma once
#include <Windows.h>

// Initialize FPV viewmodel system. Call after game.dll is loaded.
bool InitFpvViewmodel(uintptr_t gameBase);

// Shutdown and cleanup.
void ShutdownFpvViewmodel();

// Call each frame during render to draw the spectated player's FPV weapon.
bool FpvViewmodel_RenderFrame(void* playerEntity);

// Get our FPV hands skeleton pointer (for positioning from camera hook)
void* FpvViewmodel_GetSkeleton();
