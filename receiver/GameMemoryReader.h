#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include "SpectatorController.h"

struct MemoryPlayerInfo {
    int playerId;
    std::string name;
    int teamId;
    int kills;
    int deaths;
    int score;
    int ping;
};

class GameMemoryReader {
public:
    static void Init(uintptr_t baseAddr);
    static std::vector<PlayerInfo> ReadPlayerList();
    static int GetTotalKills();

private:
    static uintptr_t baseGame;
    static std::string ResolveTeamName(void* playerStruct);
    static std::string GetTeamName(int teamId);
};