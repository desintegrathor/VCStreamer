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
#include "FpvViewmodel.h"
#include "dsound_proxy.h"
#include "minhook/MinHook.h"

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

    // Suppress crash recovery: hook GAM_PTE_RUNS_Set to no-op so the game
    // doesn't write a "crashed" flag to registry. Without this, a crash
    // causes a compatibility dialog on next launch that blocks auto-restart.
    {
        HMODULE gameDll = (HMODULE)base;
        void* pteRunsSet = GetProcAddress(gameDll, "?GAM_PTE_RUNS_Set@@YAXK@Z");
        if (pteRunsSet) {
            MH_STATUS st = MH_Initialize();
            if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
                std::cout << "[VCStreamer] MinHook init failed for crash recovery\n";
            } else {
                static void* s_origPteRuns = nullptr;
                auto noop = [](unsigned long) {};  // no-op lambda
                // Use a static function since lambdas can't be used as function pointers with captures
                struct PteHook {
                    static void __cdecl Hook(unsigned long) { /* no-op */ }
                };
                if (MH_CreateHook(pteRunsSet, (void*)&PteHook::Hook, &s_origPteRuns) == MH_OK) {
                    MH_EnableHook(pteRunsSet);
                    std::cout << "[VCStreamer] Crash recovery suppressed (GAM_PTE_RUNS_Set hooked)\n";
                }
            }
        } else {
            std::cout << "[VCStreamer] GAM_PTE_RUNS_Set not found (crash recovery not suppressed)\n";
        }

        // Patch vietcong.exe to skip settings/crash-recovery dialog at startup
        HMODULE exe = GetModuleHandleA(NULL);
        if (exe) {
            BYTE* exeBase = (BYTE*)exe;
            DWORD oldProt;
            // NOP 6 bytes at exe+0x2A62 (jnz that skips dialog on no-crash)
            if (VirtualProtect(exeBase + 0x2A62, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memset(exeBase + 0x2A62, 0x90, 6);
                VirtualProtect(exeBase + 0x2A62, 6, oldProt, &oldProt);
            }
            // NOP 6 bytes at exe+0x2A75 (jz that shows dialog)
            if (VirtualProtect(exeBase + 0x2A75, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memset(exeBase + 0x2A75, 0x90, 6);
                VirtualProtect(exeBase + 0x2A75, 6, oldProt, &oldProt);
            }
            // Patch exe+0x3AA1: jz (0F 84) -> nop + jmp (90 E9) to skip crash recovery dialog
            if (VirtualProtect(exeBase + 0x3AA1, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                exeBase[0x3AA1] = 0x90; // NOP
                exeBase[0x3AA2] = 0xE9; // JMP rel32 (offset stays same)
                VirtualProtect(exeBase + 0x3AA1, 2, oldProt, &oldProt);
            }
            std::cout << "[VCStreamer] Crash recovery dialog bypassed (exe patched)\n";
        }
    }

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
    InitFpvViewmodel(base);
    InitRealtimeHook(base);
    InitAutoSpectator(base);
    InitWorldCameraTracker(base);
    WorldCameraTracker_SetPreference(2);  // Force player camera, no world cams
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
