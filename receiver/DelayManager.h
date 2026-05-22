#pragma once
#include <queue>
#include <chrono>
#include <Windows.h>

struct DelayedAction {
    enum class Type {
        KILL,
        FLAG
    };

    Type type;
    std::chrono::system_clock::time_point executeTime;
    int killerId;
    int victimId;
    int weaponId;
    int usCarrier;
    int vcCarrier;
    int leadMs;

    static DelayedAction CreateKillAction(int killer, int victim, int weapon, int delayMs, int eventLeadMs) {
        DelayedAction action;
        action.type = Type::KILL;
        action.killerId = killer;
        action.victimId = victim;
        action.weaponId = weapon;
        action.usCarrier = 0;
        action.vcCarrier = 0;
        action.leadMs = eventLeadMs;
        action.executeTime = std::chrono::system_clock::now() + std::chrono::milliseconds(delayMs);
        return action;
    }

    static DelayedAction CreateFlagAction(int us, int vc, int delayMs, int eventLeadMs) {
        DelayedAction action;
        action.type = Type::FLAG;
        action.killerId = 0;
        action.victimId = 0;
        action.weaponId = 0;
        action.usCarrier = us;
        action.vcCarrier = vc;
        action.leadMs = eventLeadMs;
        action.executeTime = std::chrono::system_clock::now() + std::chrono::milliseconds(delayMs);
        return action;
    }
};

class DelayManager {
public:
    static void Init();
    static void AddDelayedAction(const DelayedAction& action);
    static void ProcessActions();
    static int GetGameDelaySeconds();
    static void SetGameBase(uintptr_t base) { gameBase = base; }

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
    static std::queue<DelayedAction> actions;
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

    static uintptr_t gameBase;
    static int LoadDelayFromINI();
};
