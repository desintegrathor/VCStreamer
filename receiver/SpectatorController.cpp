#include "SpectatorController.h"
#include <iostream>
#include <thread>
#include <algorithm>
#include <atomic>
#include <chrono>

static uintptr_t baseGame = 0;
static std::vector<PlayerInfo> currentPlayers;
static std::map<int, int> alphabeticalIndex; // playerId ? poøadí
static std::mutex dataMutex;

// Spectator control
static std::atomic<bool> isCooldown{ false };
static std::atomic<bool> watchingFlag{ false };
static std::atomic<bool> flagThreadRunning{ false };

static int currentSpectatorIndex = -1;
static int flagCarrierUS = 0; // ID hráèe s US vlajkou
static int flagCarrierVC = 0; // ID hráèe s VC vlajkou

// ---------------------------
// Inicializace
// ---------------------------
void InitSpectatorController(uintptr_t baseGameAddr) {
    baseGame = baseGameAddr;
    std::cout << "[Spectator] Modul inicializovan (base 0x"
        << std::hex << baseGame << std::dec << ")\n";
}

// ---------------------------
// Seøazení podle jména (duplicitní jména -> podle ID)
// ---------------------------
static void UpdateAlphabeticalOrder() {
    std::vector<std::pair<std::string, int>> sorted;
    for (auto& p : currentPlayers)
        sorted.push_back({ p.name, p.id });

    std::sort(sorted.begin(), sorted.end(),
        [](auto& a, auto& b) {
            if (a.first == b.first) return a.second < b.second;
            return a.first < b.first;
        });

    alphabeticalIndex.clear();
    for (size_t i = 0; i < sorted.size(); ++i)
        alphabeticalIndex[sorted[i].second] = static_cast<int>(i);
}

// ---------------------------
// Nastavení spectatoru na konkrétního hráèe
// ---------------------------
static void SetSpectatorToPlayerId(int playerId) {
    auto it = alphabeticalIndex.find(playerId);
    if (it == alphabeticalIndex.end()) {
        std::cout << "[Spectator] ID " << playerId << " nebyl nalezen ve scoreboardu.\n";
        return;
    }

    int index = it->second;
    uintptr_t addr = baseGame + 0x7AE348;
    *(int*)addr = index;
    currentSpectatorIndex = index;

    std::cout << "[Spectator] Prepinam pohled na hrace #" << index
        << " (ID " << playerId << ")\n";
}

// ---------------------------
// Aktualizace scoreboardu
// ---------------------------
void UpdateScoreboard(const std::vector<PlayerInfo>& players) {
    std::lock_guard<std::mutex> lock(dataMutex);

    currentPlayers.clear();
    for (auto& p : players) {
        if (p.name.rfind("Spectator", 0) == 0)
            continue;
        currentPlayers.push_back(p);
    }

    UpdateAlphabeticalOrder();
    std::cout << "[Spectator] Scoreboard aktualizovan ("
        << currentPlayers.size() << " hracu)\n";
}

// ---------------------------
// Kill event – kamera na OBÌ
// ---------------------------
void ProcessKillEvent(int victimId) {
    std::lock_guard<std::mutex> lock(dataMutex);

    if (watchingFlag) {
        std::cout << "[Spectator] Ignoruji kill event – kamera sleduje vlajku.\n";
        return;
    }

    if (isCooldown) {
        std::cout << "[Spectator] Kill event ignorovan – cooldown bezi.\n";
        return;
    }

    SetSpectatorToPlayerId(victimId);

    // Nastav cooldown
    isCooldown = true;
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        isCooldown = false;
        std::cout << "[Spectator] Cooldown skoncil.\n";
        }).detach();
}

// ---------------------------
// Vnitøní vlákno sledující vlajkonoše
// ---------------------------
static void FlagWatcherThread() {
    flagThreadRunning = true;

    while (true) {
        if (!watchingFlag) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        int localUS = 0, localVC = 0;

        {
            std::lock_guard<std::mutex> lock(dataMutex);
            localUS = flagCarrierUS;
            localVC = flagCarrierVC;
        }

        if (localUS != 0 && localVC == 0) {
            SetSpectatorToPlayerId(localUS);
        }
        else if (localVC != 0 && localUS == 0) {
            SetSpectatorToPlayerId(localVC);
        }
        else if (localUS != 0 && localVC != 0) {
            // obì vlajky neseny ? pøepínej mezi nimi každých 5s
            SetSpectatorToPlayerId(localUS);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            SetSpectatorToPlayerId(localVC);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    flagThreadRunning = false;
}

// ---------------------------
// Flag event – pøepínání na hráèe s vlajkou
// ---------------------------
void ProcessFlagEvent(int usCarrier, int vcCarrier) {
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        flagCarrierUS = usCarrier;
        flagCarrierVC = vcCarrier;
    }

    if (usCarrier == 0 && vcCarrier == 0) {
        if (watchingFlag) {
            std::cout << "[Flag] Nikdo nenese vlajku – vracim kontrolu killum.\n";
        }
        watchingFlag = false;
        return;
    }

    watchingFlag = true;

    // Spustí watcher jen jednou
    if (!flagThreadRunning) {
        std::thread(FlagWatcherThread).detach();
    }
}
