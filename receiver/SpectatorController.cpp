#include "SpectatorController.h"
#include <iostream>
#include <thread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include "GameMemoryReader.h"

static uintptr_t baseGame = 0;
static std::vector<PlayerInfo> currentPlayers;
static std::map<int, int> alphabeticalIndex; // playerId ? po�ad�
static std::mutex dataMutex;

// Spectator control
static std::atomic<bool> isCooldown{ false };
static std::atomic<bool> watchingFlag{ false };
static std::atomic<bool> flagThreadRunning{ false };
static std::atomic<bool> flagPriorityTimer{ false };

static int currentSpectatorIndex = -1;
static int flagCarrierUS = 0; // ID hráče s US vlajkou
static int flagCarrierVC = 0; // ID hráče s VC vlajkou
static int lastFlagCarrierUS = 0; // Poslední známý nosič US vlajky
static int lastFlagCarrierVC = 0; // Poslední známý nosič VC vlajky

// ---------------------------
// Inicializace
// ---------------------------
void InitSpectatorController(uintptr_t baseGameAddr) {
    baseGame = baseGameAddr;
    std::cout << "[Spectator] Module initialized (base 0x"
        << std::hex << baseGame << std::dec << ")\n";
}

// ---------------------------
// Se�azen� podle jm�na (duplicitn� jm�na -> podle ID)
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
// Nastaven� spectatoru na konkr�tn�ho hr��e
// ---------------------------
static void SetSpectatorToPlayerId(int playerId) {
    // Resolve ID -> order using our local scoreboard (read from game memory) right before switching.
    auto localPlayers = GameMemoryReader::ReadPlayerList();

    std::vector<std::pair<std::string, int>> sorted;
    for (const auto& p : localPlayers) {
        sorted.emplace_back(p.name, p.id);
    }

    // Sort alphabetically by name; for duplicate names, sort by ID ascending
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.first == b.first) return a.second < b.second;
        return a.first < b.first;
    });

    int index = -1;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (sorted[i].second == playerId) {
            index = static_cast<int>(i);
            break;
        }
    }

    if (index == -1) {
        std::cout << "[Spectator] ID " << playerId << " was not found in local scoreboard.\n";
        return;
    }

    uintptr_t addr = baseGame + 0x7AE348;
    *(int*)addr = index;
    currentSpectatorIndex = index;

    // Find player name for logging
    std::string name = "Unknown";
    for (const auto& p : localPlayers) if (p.id == playerId) { name = p.name; break; }
    std::cout << "[Spectator] Switching view to player " << name << " (ID " << playerId << ")\n";
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
    std::cout << "[Spectator] Scoreboard updated ("
        << currentPlayers.size() << " players)\n";
}

// ---------------------------
// Kill event - kamera na killera
// ---------------------------
void ProcessKillEvent(int killerId, int victimId) {
    std::lock_guard<std::mutex> lock(dataMutex);

    // Pokud je aktivní flag watching nebo cooldown, jen přepneme kameru bez zpráv
    if (watchingFlag || isCooldown) {
        return;
    }

    // Najít jména hráčů
    std::string killerName = "Unknown";
    std::string victimName = "Unknown";
    
    for (const auto& player : currentPlayers) {
        if (player.id == killerId) killerName = player.name;
        if (player.id == victimId) victimName = player.name;
    }

    std::cout << "[Kill] " << killerName << " zabije " << victimName << "\n";
    SetSpectatorToPlayerId(killerId);

    // Nastav cooldown
    isCooldown = true;
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        isCooldown = false;
        }).detach();
}

// ---------------------------
// Vnit�n� vl�kno sleduj�c� vlajkono�e
// ---------------------------
static void FlagWatcherThread() {
    flagThreadRunning = true;

    while (true) {
        if (!watchingFlag) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        int localUS = 0, localVC = 0;
        int lastUS = 0, lastVC = 0;

        {
            std::lock_guard<std::mutex> lock(dataMutex);
            localUS = flagCarrierUS;
            localVC = flagCarrierVC;
            lastUS = lastFlagCarrierUS;
            lastVC = lastFlagCarrierVC;
        }

        // Používáme aktuální vlajkonoše, pokud existují
        if (localUS != 0 && localVC == 0) {
            SetSpectatorToPlayerId(localUS);
        }
        else if (localVC != 0 && localUS == 0) {
            SetSpectatorToPlayerId(localVC);
        }
        else if (localUS != 0 && localVC != 0) {
            // Obě vlajky neseny - přepínej mezi nimi každých 5s
            SetSpectatorToPlayerId(localUS);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            SetSpectatorToPlayerId(localVC);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        // Pokud nikdo nenese vlajku, ale jsme v časovém okně po ztrátě vlajky
        else if (flagPriorityTimer && (lastUS != 0 || lastVC != 0)) {
            if (lastUS != 0) {
                SetSpectatorToPlayerId(lastUS);
            }
            else if (lastVC != 0) {
                SetSpectatorToPlayerId(lastVC);
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    flagThreadRunning = false;
}

// ---------------------------
// Flag event - přepínání na hráče s vlajkou
// ---------------------------
void ProcessFlagEvent(int usCarrier, int vcCarrier) {
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        
        // Pokud některý z vlajkonošů přestal nést vlajku, uložíme si jeho ID
        if (flagCarrierUS != 0 && usCarrier == 0) {
            lastFlagCarrierUS = flagCarrierUS;
        }
        if (flagCarrierVC != 0 && vcCarrier == 0) {
            lastFlagCarrierVC = flagCarrierVC;
        }
        
        flagCarrierUS = usCarrier;
        flagCarrierVC = vcCarrier;
    }

    // Pokud někdo nese vlajku, nastavíme watching flag
    if (usCarrier != 0 || vcCarrier != 0) {
        watchingFlag = true;
        // Reset časovače priority
        flagPriorityTimer = false;
    }
    // Pokud nikdo nenese vlajku
    else if (usCarrier == 0 && vcCarrier == 0) {
        // Spustíme časovač pouze pokud jsme sledovali vlajku a časovač ještě neběží
        if (watchingFlag && !flagPriorityTimer) {
            flagPriorityTimer = true;
            std::cout << "[Flag] Flag lost - keeping camera on carrier for 3 more seconds.\n";
            
            // Spustíme časovač pro vypnutí priority
            std::thread([&]() {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                if (flagPriorityTimer) { // Kontrola, zda mezitím někdo nevzal vlajku
                    watchingFlag = false;
                    lastFlagCarrierUS = 0;
                    lastFlagCarrierVC = 0;
                    flagPriorityTimer = false;
                    std::cout << "[Flag] Timer expired - returning control to kills.\n";
                }
            }).detach();
        }
    }

    // Spustí watcher jen jednou
    if (!flagThreadRunning) {
        std::thread(FlagWatcherThread).detach();
    }
}
