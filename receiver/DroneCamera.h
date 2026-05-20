#pragma once
#include <cstdint>

// Drone camera — continuous tracking shot that flies between players.
// The CameraDirector feeds it target handles (kills, flags, etc.)
// and the drone physically flies to each new target, creating one
// unbroken camera movement through the map.

void DroneCamera_Init(uintptr_t gameBase);
void DroneCamera_Activate(const float startPos[3]);
void DroneCamera_Deactivate();
void DroneCamera_Update(float dt);
bool DroneCamera_IsActive();
void DroneCamera_GetCameraState(float* outPos, float* outPitch, float* outYaw);

// Called by CameraDirector when events happen — drone flies to this player
void DroneCamera_SetTarget(int playerHandle);
void DroneCamera_SetKillTarget(int killerHandle, int victimHandle);
int  DroneCamera_GetTargetHandle();
