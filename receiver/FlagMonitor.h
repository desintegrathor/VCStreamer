#pragma once
#include <Windows.h>

// Initialize flag monitor that polls flag carrier handles from game memory.
// Reads US flag carrier at game.dll + 0x80DDE0 and VC flag carrier at game.dll + 0x80DDE4.
// When a change is detected, calls ProcessFlagEvent().
// Must be called after game.dll is loaded.
void InitFlagMonitor(uintptr_t baseGame);

// Stop the flag monitor polling thread.
void ShutdownFlagMonitor();
