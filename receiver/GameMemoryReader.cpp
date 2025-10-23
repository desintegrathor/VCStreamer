#include "GameMemoryReader.h"
#include <iostream>
#include <thread>

uintptr_t GameMemoryReader::baseGame = 0;

void GameMemoryReader::Init(uintptr_t baseAddr) {
    // If caller provided a base address, use it. Otherwise wait for game.dll to be loaded
    baseGame = baseAddr;
    if (!baseGame) {
        std::cout << "[GameMemory] Waiting for game.dll to load...\n";
        while (!baseGame) {
            HMODULE h = GetModuleHandleW(L"game.dll");
            if (h) {
                baseGame = reinterpret_cast<uintptr_t>(h);
                std::cout << "[GameMemory] game.dll found, base = 0x" << std::hex << baseGame << std::dec << "\n";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

std::string GameMemoryReader::GetTeamName(int teamId) {
    switch (teamId) {
        case 1: return "VC";
        case 0: return "US";
        default: return "Unknown";
    }
}

std::string GameMemoryReader::ResolveTeamName(void* playerStruct) {
    if (!playerStruct) return "Unknown";
    
    int teamId = *((int*)playerStruct + 5);
    std::string teamName = GetTeamName(teamId);
    
    const char* name = (const char*)playerStruct + 40;
    std::string playerName(name);
    
    if (teamId == 0 && playerName.rfind("Spectator", 0) == 0)
        teamName = "Spectator";
        
    return teamName;
}

std::vector<PlayerInfo> GameMemoryReader::ReadPlayerList() {
    std::vector<PlayerInfo> players;
    if (!baseGame) return players;

    void** playerTable = (void**)(baseGame + 0x7AE9C8);

    for (int i = 0; i < 64; i++) {
        void* entry = playerTable[i];
        if (!entry) continue;

        const char* name = (const char*)entry + 40;
        std::string playerName(name);
        
        // Přeskoč hráče, kteří mají ve jméně "Spectator"
        if (playerName.find("Spectator") != std::string::npos) {
            continue;
        }

        PlayerInfo player;
        player.id = *(int*)entry;
        player.name = playerName;
        player.kills = *((int*)entry + 2912);
        player.deaths = *((int*)entry + 2913);
        player.score = *((int*)entry + 2914);
        players.push_back(player);
    }

    return players;
}

int GameMemoryReader::GetTotalKills() {
    auto players = ReadPlayerList();
    int total = 0;
    for (const auto& player : players) {
        total += player.kills;
    }
    return total;
}