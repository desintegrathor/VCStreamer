#pragma once
#include <queue>
#include <chrono>
#include "SpectatorController.h"

struct DelayedAction {
    enum class Type {
        KILL,
        FLAG
    };

    Type type;
    std::chrono::system_clock::time_point executeTime;
    int killerId;
    int victimId;
    int usCarrier;
    int vcCarrier;

    static DelayedAction CreateKillAction(int killer, int victim, int delayMs) {
        DelayedAction action;
        action.type = Type::KILL;
        action.killerId = killer;
        action.victimId = victim;
        action.executeTime = std::chrono::system_clock::now() + std::chrono::milliseconds(delayMs);
        return action;
    }

    static DelayedAction CreateFlagAction(int us, int vc, int delayMs) {
        DelayedAction action;
        action.type = Type::FLAG;
        action.usCarrier = us;
        action.vcCarrier = vc;
        action.executeTime = std::chrono::system_clock::now() + std::chrono::milliseconds(delayMs);
        return action;
    }
};

class DelayManager {
public:
    static void Init(uintptr_t baseGame = 0);
    static void AddDelayedAction(const DelayedAction& action);
    static void ProcessActions();
    static int GetCurrentDelay() { return currentDelay; }

    // Read spectator delay directly from game.dll memory (dword_F7DB48)
    // Returns delay in seconds as set by server via "set srvspectators <max> <delay>"
    static int GetGameDelaySeconds();

    // FPV camera offsets (loaded from INI)
    static float GetFpvOffsetBack() { return fpvOffsetBack; }
    static float GetFpvOffsetLeft() { return fpvOffsetLeft; }
    static float GetFpvOffsetUp() { return fpvOffsetUp; }
    static float GetFpvPitchOffset() { return fpvPitchOffset; }
    static int GetFpvChance() { return fpvChance; }
    static bool IsDebugMode() { return debugMode; }

    // Periodic config reload (only in debug mode)
    static void ReloadConfigIfNeeded();

private:
    static uintptr_t gameBase;
    static std::queue<DelayedAction> actions;
    static int currentDelay;  // v milisekundách (loaded from INI)

    // FPV camera offsets
    static float fpvOffsetBack;
    static float fpvOffsetLeft;
    static float fpvOffsetUp;
    static float fpvPitchOffset;
    static int fpvChance;
    static bool debugMode;

    // Config reload timing
    static DWORD lastConfigReload;
    static const DWORD CONFIG_RELOAD_INTERVAL = 5000;  // 5 seconds

    static int LoadDelayFromINI();
    static void LoadConfigFromINI();
};