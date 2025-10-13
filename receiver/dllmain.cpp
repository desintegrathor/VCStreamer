#include <Windows.h>
#include <iostream>
#include <thread>
#include <string>
#include "nlohmann/json.hpp"
#include "SpectatorController.h"

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
        std::cout << "[Receiver] CreateNamedPipe selhalo: " << GetLastError() << std::endl;
        return;
    }

    std::cout << "[Receiver] Čekám na připojení..." << std::endl;
    ConnectNamedPipe(hPipe, NULL);
    std::cout << "[Receiver] Klient připojen!" << std::endl;

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
            // KILL EVENT (kameru na OBĚŤ)
            // ---------------------------
            else if (type == "kill") {
                int victim = j["victim"];
                ProcessKillEvent(victim);
            }

            // ---------------------------
            // FLAG EVENT (vlajky)
            // ---------------------------
            else if (type == "flag") {
                int usCarrier = j["US"];
                int vcCarrier = j["VC"];
                ProcessFlagEvent(usCarrier, vcCarrier);
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[Receiver] Chyba při parsování JSON: " << e.what() << std::endl;
        }
        catch (...) {
            std::cerr << "[Receiver] Neočekávaná chyba při zpracování dat!" << std::endl;
        }
    }

    std::cout << "[Receiver] Pipe ukončeno." << std::endl;
    CloseHandle(hPipe);
}

// ---------------------------
// DLL Main Thread
// ---------------------------
DWORD WINAPI MainThread(LPVOID) {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);

    uintptr_t base = GetModuleBase(L"game.dll");
    InitSpectatorController(base);

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
