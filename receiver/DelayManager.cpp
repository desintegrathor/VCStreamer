#include "DelayManager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <Windows.h>

std::queue<DelayedAction> DelayManager::actions;
int DelayManager::currentDelay = 0;

int DelayManager::LoadDelayFromINI() {
    // Get the directory where the DLL is located
    char dllPath[MAX_PATH];
    HMODULE hModule = NULL;

    // Get handle to this DLL
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)&LoadDelayFromINI,
                          &hModule)) {
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);

        // Extract directory from full path
        std::string fullPath(dllPath);
        size_t lastSlash = fullPath.find_last_of("\\/");
        std::string directory = fullPath.substr(0, lastSlash + 1);
        std::string iniPath = directory + "vcstreamer.ini";

        // Read INI file
        std::ifstream iniFile(iniPath);
        if (iniFile.is_open()) {
            std::string line;
            while (std::getline(iniFile, line)) {
                // Skip comments and empty lines
                if (line.empty() || line[0] == ';' || line[0] == '#') {
                    continue;
                }

                // Look for delay=X or delay = X (with spaces)
                size_t equalPos = line.find('=');
                if (equalPos != std::string::npos) {
                    std::string key = line.substr(0, equalPos);
                    std::string value = line.substr(equalPos + 1);

                    // Trim whitespace
                    key.erase(0, key.find_first_not_of(" \t\r\n"));
                    key.erase(key.find_last_not_of(" \t\r\n") + 1);
                    value.erase(0, value.find_first_not_of(" \t\r\n"));
                    value.erase(value.find_last_not_of(" \t\r\n") + 1);

                    if (key == "delay") {
                        try {
                            int delaySeconds = std::stoi(value);
                            return delaySeconds * 1000; // Convert to milliseconds
                        } catch (...) {
                            // Invalid value, use default
                        }
                    }
                }
            }
            iniFile.close();
        } else {
            // Create default INI file
            std::ofstream outFile(iniPath);
            if (outFile.is_open()) {
                outFile << "; VCStreamer Configuration File\n";
                outFile << "; Spectator delay in seconds (time between scraper and streamer)\n";
                outFile << "delay=10\n";
                outFile.close();
                return 10000; // Default 10 seconds
            }
        }
    }

    // Default fallback
    return 10000;
}

void DelayManager::Init() {
    currentDelay = LoadDelayFromINI();
}

void DelayManager::AddDelayedAction(const DelayedAction& action) {
    // Pokud je zpoždění 6 sekund nebo méně, spusť akci okamžitě
    if (currentDelay <= 6000) {
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