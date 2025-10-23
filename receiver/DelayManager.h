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
    static void Init();
    static void UpdateLocalKills(int kills);
    static void UpdatePipeKills(int kills);
    static void AddDelayedAction(const DelayedAction& action);
    static void ProcessActions();
    static int GetCurrentDelay() { return currentDelay; }
    static bool IsDelayDetected() { return delayDetected; }

private:
    static std::queue<DelayedAction> actions;
    static int localKills;
    static int pipeKills;
    static int currentDelay;  // v milisekundách
    static bool delayDetected;
    static std::chrono::system_clock::time_point lastLocalKillsUpdate;
    static std::chrono::system_clock::time_point lastPipeKillsUpdate;

    static void TryDetectDelay();
};