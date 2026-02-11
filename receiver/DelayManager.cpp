#include "DelayManager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <Windows.h>

std::queue<DelayedAction> DelayManager::actions;
int DelayManager::currentDelay = 0;
float DelayManager::fpvOffsetBack = 0.8f;
float DelayManager::fpvOffsetLeft = 0.3f;
float DelayManager::fpvOffsetUp = 0.3f;
float DelayManager::fpvPitchOffset = 0.785f;
int DelayManager::fpvChance = 50;
bool DelayManager::debugMode = false;
DWORD DelayManager::lastConfigReload = 0;

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
                            currentDelay = delaySeconds * 1000; // Convert to milliseconds
                        } catch (...) {
                            // Invalid value, use default
                        }
                    } else if (key == "fpv_offset_back") {
                        try { fpvOffsetBack = std::stof(value); } catch (...) {}
                    } else if (key == "fpv_offset_left") {
                        try { fpvOffsetLeft = std::stof(value); } catch (...) {}
                    } else if (key == "fpv_offset_up") {
                        try { fpvOffsetUp = std::stof(value); } catch (...) {}
                    } else if (key == "fpv_pitch_offset") {
                        try { fpvPitchOffset = std::stof(value); } catch (...) {}
                    } else if (key == "fpv_chance") {
                        try { fpvChance = std::stoi(value); } catch (...) {}
                    } else if (key == "debug_mode") {
                        try { debugMode = (std::stoi(value) != 0); } catch (...) {}
                    }
                }
            }
            iniFile.close();
            return currentDelay != 0 ? currentDelay : 10000;
        } else {
            // Create default INI file
            std::ofstream outFile(iniPath);
            if (outFile.is_open()) {
                outFile << "; VCStreamer Configuration File\n";
                outFile << "\n";
                outFile << "; Spectator delay in seconds (time between scraper and streamer)\n";
                outFile << "delay=10\n";
                outFile << "\n";
                outFile << "; FPV camera offsets (in meters, relative to player direction)\n";
                outFile << "fpv_offset_back=0.8\n";
                outFile << "fpv_offset_left=0.3\n";
                outFile << "fpv_offset_up=0.3\n";
                outFile << "\n";
                outFile << "; FPV pitch offset in radians (0.785 = ~45 degrees down)\n";
                outFile << "fpv_pitch_offset=0.785\n";
                outFile << "\n";
                outFile << "; FPV chance in percent (0-100, chance of FPV vs 3PV when switching players)\n";
                outFile << "fpv_chance=50\n";
                outFile << "\n";
                outFile << "; Debug mode (0=normal, 1=reload config periodically for live tuning)\n";
                outFile << "debug_mode=0\n";
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

void DelayManager::ReloadConfigIfNeeded() {
    // Only reload periodically in debug mode
    if (!debugMode) {
        return;
    }

    DWORD now = GetTickCount();
    if (now - lastConfigReload < CONFIG_RELOAD_INTERVAL) {
        return;  // Too soon since last reload
    }
    lastConfigReload = now;

    // Re-read config (LoadDelayFromINI also loads FPV config)
    LoadDelayFromINI();
}
