#include <Windows.h>
#include <iostream>
#include <thread>
#include <string>
#include "SpectatorController.h"
#include "GameMemoryReader.h"
#include "DelayManager.h"
#include "CameraDirector.h"
#include "FirstPersonCamera.h"
#include "AutoSpectator.h"
#include "WorldCameraTracker.h"
#include "RealtimeHook.h"
#include "TickDelayBuffer.h"
#include "dsound_proxy.h"

// ---------------------------
// Získání base adresy modulu
// ---------------------------
uintptr_t GetModuleBase(const wchar_t* moduleName) {
    HMODULE hMod = GetModuleHandleW(moduleName);
    return hMod ? reinterpret_cast<uintptr_t>(hMod) : 0;
}

// ---------------------------
// Main loop: scoreboard polling + delayed action processing + camera director update
// ---------------------------
void MainLoop() {
    while (true) {
        // Process scheduled actions (kills/flags with delay timing)
        DelayManager::ProcessActions();

        // Update camera director state machine
        CameraDirector_Update();

        // Read scoreboard from game memory and update SpectatorController
        auto players = GameMemoryReader::ReadPlayerList();
        if (!players.empty()) {
            UpdateScoreboard(players);
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

    // Wait for game.dll to fully initialize.
    // Proxy DLL loads earlier than the old injection method, so we need a longer wait.
    std::cout << "[VCStreamer] Waiting for game initialization...\n";
    std::this_thread::sleep_for(std::chrono::seconds(8));

    InitSpectatorController(base);
    GameMemoryReader::Init(base);
    DelayManager::SetGameBase(base);
    DelayManager::Init();

    InitCameraDirector(base);
    InitTickDelayBuffer(base);
    InitFirstPersonCamera(base);
    InitRealtimeHook(base);
    InitAutoSpectator(base);
    InitWorldCameraTracker(base);
    SetHookReady();

    std::thread mainThread(MainLoop);
    mainThread.detach();

    return 0;
}

// ---------------------------
// DllMain
// ---------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadRealDsound();
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
