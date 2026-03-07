#define _CRT_SECURE_NO_WARNINGS
#include "FlagMonitor.h"
#include "SpectatorController.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <windows.h>

static FILE* g_flagLog = nullptr;

static void FlagLog(const char* fmt, ...) {
    if (!g_flagLog) {
        g_flagLog = fopen("flag_debug.log", "w");
        if (!g_flagLog) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_flagLog, fmt, args);
    fflush(g_flagLog);
    va_end(args);
}

// Flag carrier handle addresses (offsets from game.dll base)
constexpr uintptr_t OFFSET_US_FLAG_CARRIER = 0x80DDE0;
constexpr uintptr_t OFFSET_VC_FLAG_CARRIER = 0x80DDE4;

// Player table for resolving handles to player IDs
constexpr uintptr_t OFFSET_PLAYER_TABLE = 0x7AE9C8;

static uintptr_t g_baseGame = 0;
static std::atomic<bool> g_running{ false };
static std::thread g_pollThread;

// Last known flag carrier handles
static int g_lastUSCarrier = 0;
static int g_lastVCCarrier = 0;

static void FlagPollLoop() {
    std::cout << "[FlagMonitor] Polling thread started\n";

    // Set hardware watchpoints on this thread — but game writes happen on the game thread.
    // We need to set them on the main thread instead.

    while (g_running) {
        int usCarrier = *(int*)(g_baseGame + OFFSET_US_FLAG_CARRIER);
        int vcCarrier = *(int*)(g_baseGame + OFFSET_VC_FLAG_CARRIER);

        if (usCarrier != g_lastUSCarrier || vcCarrier != g_lastVCCarrier) {
            FlagLog("[FlagMonitor] TICK=%lu Flag change: US=%d (was %d) VC=%d (was %d)\n",
                    GetTickCount(), usCarrier, g_lastUSCarrier, vcCarrier, g_lastVCCarrier);
            std::cout << "[FlagMonitor] Flag change: US=" << usCarrier
                      << " VC=" << vcCarrier << "\n";

            g_lastUSCarrier = usCarrier;
            g_lastVCCarrier = vcCarrier;

            ProcessFlagEvent(usCarrier, vcCarrier);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "[FlagMonitor] Polling thread stopped\n";
}

void InitFlagMonitor(uintptr_t baseGame) {
    g_baseGame = baseGame;
    g_lastUSCarrier = 0;
    g_lastVCCarrier = 0;

    uintptr_t usAddr = baseGame + OFFSET_US_FLAG_CARRIER;
    uintptr_t vcAddr = baseGame + OFFSET_VC_FLAG_CARRIER;

    std::cout << "[FlagMonitor] Initializing flag monitor...\n";
    std::cout << "[FlagMonitor] US flag addr: 0x" << std::hex
              << usAddr << "\n";
    std::cout << "[FlagMonitor] VC flag addr: 0x"
              << vcAddr << std::dec << "\n";

    g_running = true;
    g_pollThread = std::thread(FlagPollLoop);
    g_pollThread.detach();
}

void ShutdownFlagMonitor() {
    g_running = false;
    std::cout << "[FlagMonitor] Shutdown requested\n";
    if (g_flagLog) { fclose(g_flagLog); g_flagLog = nullptr; }
}
