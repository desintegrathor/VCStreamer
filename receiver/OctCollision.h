#pragma once
#include <cstdint>

// Call once after game.dll and logs.dll are loaded
void OctCollision_Init(uintptr_t baseGame);

// Call when map changes — loads col.oct from game VFS
bool OctCollision_LoadCurrentMap();

// Cast a ray, return distance to nearest hit (FLT_MAX if none)
float OctCollision_Raycast(const float* origin, const float* dir, float* hitNormal = nullptr);

// Returns true if unobstructed line of sight between two points
bool OctCollision_LineOfSight(const float* from, const float* to);

// Check if collision data is loaded
bool OctCollision_IsLoaded();
