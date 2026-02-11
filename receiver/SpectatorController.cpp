#include "SpectatorController.h"
#include <iostream>
#include <thread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include "GameMemoryReader.h"

static uintptr_t baseGame = 0;
static std::vector<PlayerInfo> currentPlayers;
static std::map<int, int> alphabeticalIndex; // playerId -> pořadí
static std::mutex dataMutex;

// Spectator control
static std::atomic<bool> isCooldown{ false };
static std::atomic<bool> watchingFlag{ false };
static std::atomic<bool> flagThreadRunning{ false };
static std::atomic<bool> flagPriorityTimer{ false };

static int currentSpectatorIndex = -1;
static int currentSpectatorPlayerId = -1; // Currently watched player ID
static int flagCarrierUS = 0; // ID hráče s US vlajkou
static int flagCarrierVC = 0; // ID hráče s VC vlajkou
static int lastFlagCarrierUS = 0; // Poslední známý nosič US vlajky
static int lastFlagCarrierVC = 0; // Poslední známý nosič VC vlajky

// ---------------------------
// Inicializace
// ---------------------------
void InitSpectatorController(uintptr_t baseGameAddr) {
    baseGame = baseGameAddr;

    // Patch camera distance on startup
    // Using shorter distances for better visibility: 3.0m normal, 1.5m crouched
    PatchCameraDistance(3.0f, 1.5f);
}

// ---------------------------
// Seřazení podle jména (duplicitní jména -> podle ID)
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
// Nastavení spectatoru na konkrétního hráče
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
    currentSpectatorPlayerId = playerId;  // Remember which player we're watching

    // Find player name for logging
    std::string name = "Unknown";
    for (const auto& p : localPlayers) if (p.id == playerId) { name = p.name; break; }
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

    std::cout << "[Kill] " << killerName << " killed " << victimName << " -> switching to " << killerName << "\n";
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

        // Get player names for logging
        std::string usName = "Unknown", vcName = "Unknown";
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            for (const auto& p : currentPlayers) {
                if (p.id == localUS) usName = p.name;
                if (p.id == localVC) vcName = p.name;
            }
        }

        // Používáme aktuální vlajkonoše, pokud existují
        if (localUS != 0 && localVC == 0) {
            std::cout << "[Flag] Following US flag carrier: " << usName << "\n";
            SetSpectatorToPlayerId(localUS);
        }
        else if (localVC != 0 && localUS == 0) {
            std::cout << "[Flag] Following VC flag carrier: " << vcName << "\n";
            SetSpectatorToPlayerId(localVC);
        }
        else if (localUS != 0 && localVC != 0) {
            // Obě vlajky neseny - přepínej mezi nimi každých 5s
            std::cout << "[Flag] Both flags taken - alternating between " << usName << " (US) and " << vcName << " (VC)\n";
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

// ---------------------------
// Camera Orientation Control
// ---------------------------

void SetCameraOrientation(float pitch, float yaw, float roll) {
    if (!baseGame) return;

    // Based on spectator_orientation_lock.txt documentation:
    // We need to find the "free camera entry" structure and write rotation there
    // The structure contains rotation at offsets:
    // +0x8  = pitch (rotation_x)
    // +0xC  = yaw (rotation_y)
    // +0x10 = distance (we don't modify this here)

    // For spectator mode 2 (Free Camera), the camera rotation is stored in a structure
    // that we need to find. According to the docs, this is loaded during GNET_SpectatorCtrl_FillCamera
    // For now, we'll try writing to player entity rotation which might be mirrored to camera

    // Alternative approach: Write directly to the spectator camera structure
    // This needs to be discovered through reverse engineering
    // Placeholder addresses - these need to be verified!

    // TODO: Find actual camera rotation addresses through memory scanning
    // For now, this function is prepared but needs proper addresses

    std::cout << "[Camera] Setting orientation: pitch=" << pitch
              << " yaw=" << yaw << " roll=" << roll << "\n";
}

void UpdateCameraOrientation() {
    // Placeholder for future camera orientation sync
    // Currently disabled - no debug output
}

// ---------------------------
// Camera Distance Patch
// ---------------------------

void PatchCameraDistance(float normalDistance, float crouchedDistance) {
    if (!baseGame) return;

    // Based on spectator_camera_distance.txt documentation:
    // Function: GPLAYER_CalculateFollowCameraPosition at 0x1012E570
    //
    // Normal stance distance (default 7.0m):
    //   Address: 0x1012E60D (RVA in game.dll)
    //   Original bytes: C7 44 24 18 00 00 E0 40
    //   This is: mov [esp+50h+var_38], 0x40E00000  (7.0f)
    //
    // Crouched distance (default 2.0m):
    //   Address: 0x1012E617 (RVA in game.dll)
    //   Original bytes: C7 44 24 18 00 00 00 40
    //   This is: mov [esp+50h+var_38], 0x40000000  (2.0f)

    uintptr_t normalDistAddr = baseGame + 0x1012E60D + 4;  // +4 to skip to the immediate value
    uintptr_t crouchedDistAddr = baseGame + 0x1012E617 + 4;

    DWORD oldProtect;

    // Patch normal distance
    if (VirtualProtect((LPVOID)normalDistAddr, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *(float*)normalDistAddr = normalDistance;
        VirtualProtect((LPVOID)normalDistAddr, sizeof(float), oldProtect, &oldProtect);
    }

    // Patch crouched distance
    if (VirtualProtect((LPVOID)crouchedDistAddr, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *(float*)crouchedDistAddr = crouchedDistance;
        VirtualProtect((LPVOID)crouchedDistAddr, sizeof(float), oldProtect, &oldProtect);
    }
}
