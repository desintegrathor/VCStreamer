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

        // Read player rotation (offsets in bytes: +672, +676, +680)
        // Converting to float pointer for proper reading
        float* entryFloat = (float*)entry;
        player.pitch = *(entryFloat + 168);  // +672 bytes / 4 = offset 168
        player.yaw = *(entryFloat + 169);    // +676 bytes / 4 = offset 169
        player.roll = *(entryFloat + 170);   // +680 bytes / 4 = offset 170

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

float GameMemoryReader::ReadCameraYaw() {
    if (!baseGame) return 0.0f;

    // Based on documentation: Vietcong - Spectator Mode 2 Camera Yaw Analysis
    // Global structure base: 0x14EE320
    // Free camera entry array starts at: 0x14EE350 (base + 0x30)
    // Entry size: 20 bytes (0x14)
    // Yaw offset within entry: +0x0C
    //
    // For camera index 0:
    //   Entry base = 0x14EE350 + (0 * 20) = 0x14EE350
    //   Yaw address = 0x14EE350 + 0x0C = 0x14EE35C
    //
    // Full offset from game.dll base: 0xAEE35C

    uintptr_t cameraYawAddr = baseGame + 0xAEE35C;

    __try {
        float yaw = *(float*)cameraYawAddr;
        return yaw;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}