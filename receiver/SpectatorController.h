#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>

struct PlayerInfo {
    int id = 0;
    std::string name = "";
    int kills = 0;
    int deaths = 0;
    int score = 0;
    float pitch = 0.0f;  // Rotation X (vertical look)
    float yaw = 0.0f;    // Rotation Y (horizontal look)
    float roll = 0.0f;   // Rotation Z (usually 0)
};

void InitSpectatorController(uintptr_t baseGameAddr);
void UpdateScoreboard(const std::vector<PlayerInfo>& players);
void ProcessKillEvent(int killerId, int victimId);
void ProcessFlagEvent(int usCarrier, int vcCarrier);

// Camera orientation control
void SetCameraOrientation(float pitch, float yaw, float roll);
void UpdateCameraOrientation();  // Called continuously to sync with current player

// Camera distance patch
void PatchCameraDistance(float normalDistance, float crouchedDistance);
