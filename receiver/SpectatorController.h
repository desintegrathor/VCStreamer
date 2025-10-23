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
};

void InitSpectatorController(uintptr_t baseGameAddr);
void UpdateScoreboard(const std::vector<PlayerInfo>& players);
void ProcessKillEvent(int killerId, int victimId);
void ProcessFlagEvent(int usCarrier, int vcCarrier);
