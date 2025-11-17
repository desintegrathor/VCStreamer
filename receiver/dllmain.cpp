#include <Windows.h>
#include <iostream>
#include <thread>
#include <string>
#include "nlohmann/json.hpp"
#include "SpectatorController.h"
#include "GameMemoryReader.h"
#include "DelayManager.h"

using json = nlohmann::json;

// ---------------------------
// Získání base adresy modulu
// ---------------------------
uintptr_t GetModuleBase(const wchar_t* moduleName) {
    HMODULE hMod = GetModuleHandleW(moduleName);
    return hMod ? reinterpret_cast<uintptr_t>(hMod) : 0;
}

// ---------------------------
// PIPE Listener
// ---------------------------
void PipeListener() {
    HANDLE hPipe = CreateNamedPipeW(
        L"\\\\.\\pipe\\GameDataPipe",
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 0, 0, 0, NULL
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        return;
    }

    ConnectNamedPipe(hPipe, NULL);

    char buffer[4096];
    DWORD bytesRead;

    while (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
        buffer[bytesRead] = 0;

        try {
            auto j = json::parse(buffer);
            std::string type = j["type"];

            // ---------------------------
            // SCOREBOARD EVENT
            // ---------------------------
            if (type == "scoreboard") {
                std::vector<PlayerInfo> players;

                for (auto& p : j["players"]) {
                    players.push_back({
                        p["id"],
                        p["name"],
                        p["kills"],
                        p["deaths"],
                        p["score"]
                        });
                }

                UpdateScoreboard(players);
            }

            // ---------------------------
            // KILL EVENT (kameru na KILLERA)
            // ---------------------------
            else if (type == "kill") {
                int killer = j["killer"];
                int victim = j["victim"];

                // Pro zpoždění > 6s odečteme 5s, pro menší zpoždění se akce spustí hned
                int currentDelay = DelayManager::GetCurrentDelay();
                int executeDelay = (currentDelay > 6000) ? (currentDelay - 5000) : 0;

                auto action = DelayedAction::CreateKillAction(killer, victim, executeDelay);
                DelayManager::AddDelayedAction(action);
            }

            // ---------------------------
            // FLAG EVENT (vlajky)
            // ---------------------------
            else if (type == "flag") {
                int usCarrier = j["US"];
                int vcCarrier = j["VC"];

                // Pro zpoždění > 6s odečteme 5s, pro menší zpoždění se akce spustí hned
                int currentDelay = DelayManager::GetCurrentDelay();
                int executeDelay = (currentDelay > 6000) ? (currentDelay - 5000) : 0;

                auto action = DelayedAction::CreateFlagAction(usCarrier, vcCarrier, executeDelay);
                DelayManager::AddDelayedAction(action);
            }
        }
        catch (...) {
            // Ignore parse errors silently
        }
    }

    CloseHandle(hPipe);
}

// ---------------------------
// DLL Main Thread
// ---------------------------
void ScoreboardMonitor() {
    while (true) {
        // Zpracuj čekající akce
        DelayManager::ProcessActions();

        // Update camera orientation to match current player view
        UpdateCameraOrientation();

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10x za sekundu
    }
}

DWORD WINAPI MainThread(LPVOID) {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);

    uintptr_t base = GetModuleBase(L"game.dll");
    InitSpectatorController(base);
    GameMemoryReader::Init(base);
    DelayManager::Init();

    // Spusť monitoring scoreboardu
    std::thread scoreboardThread(ScoreboardMonitor);
    scoreboardThread.detach();

    // Spusť PIPE listener
    std::thread pipeThread(PipeListener);
    pipeThread.detach();

    return 0;
}

// ---------------------------
// DllMain
// ---------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
