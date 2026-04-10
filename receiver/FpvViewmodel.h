#pragma once
#include <Windows.h>

// Initialize FPV viewmodel system. Call after game.dll is loaded.
bool InitFpvViewmodel(uintptr_t gameBase);

// Shutdown and cleanup.
void ShutdownFpvViewmodel();

// Call each frame during render to draw the spectated player's FPV weapon.
// playerEntity = the spectated player's entity pointer.
// Returns true if FPV was rendered this frame.
bool FpvViewmodel_RenderFrame(void* playerEntity);
