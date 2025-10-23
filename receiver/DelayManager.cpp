#include "DelayManager.h"
#include <iostream>

std::queue<DelayedAction> DelayManager::actions;
int DelayManager::localKills = 0;
int DelayManager::pipeKills = 0;
int DelayManager::currentDelay = 0;
bool DelayManager::delayDetected = false;
std::chrono::system_clock::time_point DelayManager::lastLocalKillsUpdate;
std::chrono::system_clock::time_point DelayManager::lastPipeKillsUpdate;

void DelayManager::Init() {
    localKills = 0;
    pipeKills = 0;
    currentDelay = 0;
    delayDetected = false;
    lastLocalKillsUpdate = std::chrono::system_clock::now();
    lastPipeKillsUpdate = std::chrono::system_clock::now();
}

void DelayManager::UpdateLocalKills(int kills) {
    localKills = kills;
    lastLocalKillsUpdate = std::chrono::system_clock::now();
    TryDetectDelay();
}

void DelayManager::UpdatePipeKills(int kills) {
    pipeKills = kills;
    lastPipeKillsUpdate = std::chrono::system_clock::now();
    TryDetectDelay();
}

void DelayManager::TryDetectDelay() {
    if (localKills > 0 && pipeKills > 0) {
        if (localKills == pipeKills) {
            auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(
                lastLocalKillsUpdate - lastPipeKillsUpdate).count();
            
            if (timeDiff > 0 && timeDiff <= 190000) { // max 190 sekund
                currentDelay = static_cast<int>(timeDiff);
                delayDetected = true;
                std::cout << "[Delay] Detected delay: " << (currentDelay / 1000.0) << " seconds\n";
            }
        }
    }
}

void DelayManager::AddDelayedAction(const DelayedAction& action) {
    if (!delayDetected) {
        std::cout << "[Delay] Cannot add action - delay not yet detected\n";
        return;
    }

    // Pokud je zpoždění 6 sekund nebo méně, spusť akci okamžitě
    if (currentDelay <= 6000) {
        std::cout << "[Delay] Delay is " << (currentDelay / 1000.0) 
                 << "s - executing action immediately\n";
        
        switch (action.type) {
            case DelayedAction::Type::KILL:
                ProcessKillEvent(action.killerId, action.victimId);
                break;
                
            case DelayedAction::Type::FLAG:
                ProcessFlagEvent(action.usCarrier, action.vcCarrier);
                break;
        }
        return;
    }

    actions.push(action);
}

void DelayManager::ProcessActions() {
    if (!delayDetected) return;

    auto now = std::chrono::system_clock::now();
    
    while (!actions.empty()) {
        const auto& action = actions.front();
        
        if (action.executeTime <= now) {
            switch (action.type) {
                case DelayedAction::Type::KILL:
                    ProcessKillEvent(action.killerId, action.victimId);
                    break;
                    
                case DelayedAction::Type::FLAG:
                    ProcessFlagEvent(action.usCarrier, action.vcCarrier);
                    break;
            }
            actions.pop();
        } else {
            break;
        }
    }
}