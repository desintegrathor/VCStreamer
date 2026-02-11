#include "FlagMonitor.h"
#include "SpectatorController.h"
#include <iostream>
#include <thread>
#include <atomic>

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

    while (g_running) {
        // Read flag carrier handles directly from game memory
        // These addresses are updated when messages are dequeued from the delay buffer,
        // so they reflect the "delayed" state that the spectator sees.
        // This is acceptable because flag events are less time-critical than kills.
        int usCarrier = *(int*)(g_baseGame + OFFSET_US_FLAG_CARRIER);
        int vcCarrier = *(int*)(g_baseGame + OFFSET_VC_FLAG_CARRIER);

        // Detect change
        if (usCarrier != g_lastUSCarrier || vcCarrier != g_lastVCCarrier) {
            std::cout << "[FlagMonitor] Flag change: US=" << usCarrier
                      << " VC=" << vcCarrier << "\n";

            g_lastUSCarrier = usCarrier;
            g_lastVCCarrier = vcCarrier;

            // Process the flag event - handles are player IDs
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

    std::cout << "[FlagMonitor] Initializing flag monitor...\n";
    std::cout << "[FlagMonitor] US flag addr: 0x" << std::hex
              << (baseGame + OFFSET_US_FLAG_CARRIER) << "\n";
    std::cout << "[FlagMonitor] VC flag addr: 0x"
              << (baseGame + OFFSET_VC_FLAG_CARRIER) << std::dec << "\n";

    g_running = true;
    g_pollThread = std::thread(FlagPollLoop);
    g_pollThread.detach();
}

void ShutdownFlagMonitor() {
    g_running = false;
    // Thread is detached, it will exit on its own when g_running becomes false
    std::cout << "[FlagMonitor] Shutdown requested\n";
}
