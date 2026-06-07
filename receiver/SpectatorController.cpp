#include "SpectatorController.h"
#include "CommentaryEngine.h"
#include "DiagnosticsLog.h"
#include <mutex>

static uintptr_t baseGame = 0;
static std::vector<PlayerInfo> currentPlayers;
static std::mutex dataMutex;

static int currentSpectatorIndex = -1;
static int currentSpectatorPlayerId = -1;

// ---------------------------
// Inicializace
// ---------------------------
void InitSpectatorController(uintptr_t baseGameAddr) {
    baseGame = baseGameAddr;
    PatchCameraDistance(3.0f, 1.5f);
}

// ---------------------------
// Nastavení spectatoru na konkrétního hráče
// ---------------------------
void SetSpectatorToPlayerId(int playerId) {
    uintptr_t spectObj = baseGame + 0x7AE320;
    int playerCount = *(int*)(spectObj + 0x24);

    if (playerCount <= 0 || playerCount > 64) return;

    uintptr_t listBase = spectObj + 0x2C;
    for (int i = 0; i < playerCount; i++) {
        int handle = *(int*)(listBase + i * 20);
        if (handle == playerId) {
            *(int*)(spectObj + 0x28) = i;
            currentSpectatorIndex = i;
            currentSpectatorPlayerId = playerId;
            return;
        }
    }
    DiagnosticsLog_Append("receiver_debug.log", "[Spectator] Handle %d not in spectator list\n", playerId);
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
    CommentaryEngine_UpdateScoreboard(currentPlayers);
}

// ---------------------------
// Camera Distance Patch
// ---------------------------

void PatchCameraDistance(float normalDistance, float crouchedDistance) {
    if (!baseGame) return;

    uintptr_t normalDistAddr = baseGame + 0x1012E60D + 4;
    uintptr_t crouchedDistAddr = baseGame + 0x1012E617 + 4;

    DWORD oldProtect;

    if (VirtualProtect((LPVOID)normalDistAddr, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *(float*)normalDistAddr = normalDistance;
        VirtualProtect((LPVOID)normalDistAddr, sizeof(float), oldProtect, &oldProtect);
    }

    if (VirtualProtect((LPVOID)crouchedDistAddr, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *(float*)crouchedDistAddr = crouchedDistance;
        VirtualProtect((LPVOID)crouchedDistAddr, sizeof(float), oldProtect, &oldProtect);
    }
}
