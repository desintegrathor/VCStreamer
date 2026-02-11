#include <Windows.h>
#include <iostream>
#include <thread>
#include <string>
#include "SpectatorController.h"
#include "GameMemoryReader.h"
#include "DelayManager.h"
#include "FirstPersonCamera.h"
#include "RealtimeHook.h"
#include "FlagMonitor.h"
#include "AutoSpectator.h"

// ---------------------------
// Získání base adresy modulu
// ---------------------------
uintptr_t GetModuleBase(const wchar_t* moduleName) {
    HMODULE hMod = GetModuleHandleW(moduleName);
    return hMod ? reinterpret_cast<uintptr_t>(hMod) : 0;
}

// ---------------------------
// DLL Main Thread
// ---------------------------
void ScoreboardMonitor() {
    DWORD lastPlayerUpdate = 0;

    while (true) {
        // Process scheduled actions (kills with delay-5s timing)
        DelayManager::ProcessActions();

        // Update camera orientation to match current player view
        UpdateCameraOrientation();

        // Periodically refresh player list so ProcessKillEvent can resolve names
        DWORD now = GetTickCount();
        if (now - lastPlayerUpdate > 2000) {
            auto players = GameMemoryReader::ReadPlayerList();
            UpdateScoreboard(players);
            lastPlayerUpdate = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10x za sekundu
    }
}

DWORD WINAPI MainThread(LPVOID) {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);

    // Wait for game.dll to be loaded
    std::cout << "[VCStreamer] Waiting for game.dll...\n";
    uintptr_t base = 0;
    for (int i = 0; i < 300; i++) {  // Max 30 seconds
        base = GetModuleBase(L"game.dll");
        if (base != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (base == 0) {
        std::cout << "[VCStreamer] ERROR: game.dll not found!\n";
        return 1;
    }

    std::cout << "[VCStreamer] game.dll found at 0x" << std::hex << base << std::dec << "\n";

    // Wait additional time for game.dll to fully initialize
    std::cout << "[VCStreamer] Waiting for game initialization...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    InitSpectatorController(base);
    GameMemoryReader::Init(base);
    DelayManager::Init(base);

    // Initialize first-person camera hook
    InitFirstPersonCamera(base);

    // Initialize realtime message hook (intercepts messages before delay buffer)
    InitRealtimeHook(base);

    // Initialize flag monitor (polls flag carrier handles from memory)
    InitFlagMonitor(base);

    // Signal that all initialization is complete and the hook can process messages
    SetHookReady();

    // Start auto-spectator (polls game state and auto-joins spectator)
    InitAutoSpectator(base);

    // Spusť monitoring scoreboardu
    std::thread scoreboardThread(ScoreboardMonitor);
    scoreboardThread.detach();

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
