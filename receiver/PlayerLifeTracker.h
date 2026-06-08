#pragma once

#include "ServerStateSniffer.h"
#include <Windows.h>
#include <vector>

struct PlayerInfo;

enum class PlayerLifeStatus {
    Alive,
    RecentlyDied,
    RespawningOrUnknown
};

struct PlayerLifeState {
    PlayerLifeStatus status = PlayerLifeStatus::RespawningOrUnknown;
    DWORD deathTick = 0;
    int killerHandle = 0;
    DWORD lastConfirmedAliveTick = 0;
};

void InitPlayerLifeTracker(uintptr_t gameBase);
void PlayerLifeTracker_OnKill(const DirectorEvent& event);
void PlayerLifeTracker_UpdateFromScoreboard(const std::vector<PlayerInfo>& players);
void PlayerLifeTracker_UpdateFromFrame(DWORD now);
PlayerLifeState PlayerLifeTracker_Get(int handle);
bool PlayerLifeTracker_IsAlive(int handle);
