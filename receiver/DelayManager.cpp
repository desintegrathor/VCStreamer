#include "DelayManager.h"
#include "CameraDirector.h"
#include "TickDelayBuffer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <mutex>
#include <Windows.h>

static std::mutex g_actionsMutex;

std::queue<DelayedAction> DelayManager::actions;
uintptr_t DelayManager::gameBase = 0;
float DelayManager::fpvOffsetBack = 0.8f;
float DelayManager::fpvOffsetLeft = 0.3f;
float DelayManager::fpvOffsetUp = 0.3f;
float DelayManager::fpvPitchOffset = 0.785f;
int DelayManager::fpvChance = 10;
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

                    if (key == "fpv_offset_back") {
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
            return 0;
        } else {
            // Create default INI file
            std::ofstream outFile(iniPath);
            if (outFile.is_open()) {
                outFile << "; VCStreamer Configuration File\n";
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
                outFile << "fpv_chance=10\n";
                outFile << "\n";
                outFile << "; Debug mode (0=normal, 1=reload config periodically for live tuning)\n";
                outFile << "debug_mode=0\n";
                outFile << "\n";
                outFile << "; Auto-connect / restart target (set auto_connect=0 to wait for manual connection)\n";
                outFile << "auto_connect=1\n";
                outFile << "server_ip=46.13.190.168\n";
                outFile << "server_port=5425\n";
                outFile << "server_addon=fistalpha\n";
                outFile << "auto_connect_retry_seconds=30\n";
                outFile << "\n";
                outFile << "; Low-player stream holding overlay\n";
                outFile << "low_player_overlay_threshold=2\n";
                outFile << "low_player_overlay_image=stream_waiting.bmp\n";
                outFile << "low_player_overlay_fade_seconds=1.5\n";
                outFile.close();
                return 0;
            }
        }
    }

    // Default fallback
    return 0;
}

void DelayManager::Init() {
    LoadDelayFromINI();
}

int DelayManager::GetGameDelaySeconds() {
    // Read server-configured spectator delay from game memory
    // dword_F7DB48 (IDA addr), RVA = 0x20DB48
    if (!gameBase) return 0;
    constexpr uintptr_t SPECT_DELAY_OFFSET = 0x20DB48;
    __try {
        return *(int*)(gameBase + SPECT_DELAY_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void DelayManager::AddDelayedAction(const DelayedAction& action) {
    // If total delay (game + buffer) is 6s or less, execute immediately
    int totalDelayMs = GetGameDelaySeconds() * 1000 + GetTickDelayMs();
    if (totalDelayMs <= 6000) {
        switch (action.type) {
            case DelayedAction::Type::KILL:
                CameraDirector_OnKill(action.killerId, action.victimId);
                break;

            case DelayedAction::Type::FLAG:
                CameraDirector_OnFlagChanged(action.usCarrier, action.vcCarrier);
                break;
        }
        return;
    }

    std::lock_guard<std::mutex> lock(g_actionsMutex);
    actions.push(action);
}

void DelayManager::ProcessActions() {
    std::lock_guard<std::mutex> lock(g_actionsMutex);
    auto now = std::chrono::system_clock::now();

    while (!actions.empty()) {
        const auto& action = actions.front();

        if (action.executeTime <= now) {
            switch (action.type) {
                case DelayedAction::Type::KILL:
                    CameraDirector_OnKill(action.killerId, action.victimId);
                    break;

                case DelayedAction::Type::FLAG:
                    CameraDirector_OnFlagChanged(action.usCarrier, action.vcCarrier);
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

// ============================================================================
// CameraDirector config loading (follows same INI pattern)
// ============================================================================

static std::string GetIniPath() {
    char dllPath[MAX_PATH];
    HMODULE hModule = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)&GetIniPath,
                          &hModule)) {
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);
        std::string fullPath(dllPath);
        size_t lastSlash = fullPath.find_last_of("\\/");
        return fullPath.substr(0, lastSlash + 1) + "vcstreamer.ini";
    }
    return "vcstreamer.ini";
}

static float ReadIniFloat(const std::string& iniPath, const char* key, float defaultVal) {
    char buf[64];
    GetPrivateProfileStringA("CameraDirector", key, "", buf, sizeof(buf), iniPath.c_str());
    if (buf[0] == '\0') return defaultVal;
    try { return std::stof(buf); } catch (...) { return defaultVal; }
}

static int ReadIniInt(const std::string& iniPath, const char* key, int defaultVal) {
    char buf[64];
    GetPrivateProfileStringA("CameraDirector", key, "", buf, sizeof(buf), iniPath.c_str());
    if (buf[0] == '\0') return defaultVal;
    try { return std::stoi(buf); } catch (...) { return defaultVal; }
}

void LoadCameraDirectorConfig(CameraConfig& cfg) {
    std::string iniPath = GetIniPath();

    cfg.cinematicMinDistance       = ReadIniFloat(iniPath, "cinematic_min_distance", 35.0f);
    cfg.cinematicMaxDistance       = ReadIniFloat(iniPath, "cinematic_max_distance", 65.0f);
    cfg.cinematicMinChance        = ReadIniFloat(iniPath, "cinematic_min_chance", 0.05f);
    cfg.cinematicMaxChance        = ReadIniFloat(iniPath, "cinematic_max_chance", 0.25f);

    cfg.killCamWaitDuration       = ReadIniFloat(iniPath, "killcam_wait_duration", 3.5f);
    cfg.killCamTransitionDuration = ReadIniFloat(iniPath, "killcam_transition_duration", 1.5f);
    cfg.killCamAttachedDuration   = ReadIniFloat(iniPath, "killcam_attached_duration", 4.0f);
    cfg.killCamSlideHeight        = ReadIniFloat(iniPath, "killcam_slide_height", 0.8f);

    cfg.killCooldown              = ReadIniFloat(iniPath, "kill_cooldown", 15.0f);
    cfg.flagLostGracePeriod       = ReadIniFloat(iniPath, "flag_lost_grace_period", 7.0f);

    cfg.worldCamMaxDistance       = ReadIniFloat(iniPath, "worldcam_max_distance", 22.0f);
    cfg.worldCamSwitchCooldown    = ReadIniFloat(iniPath, "worldcam_switch_cooldown", 12.0f);
    cfg.worldCamMaxHold           = ReadIniFloat(iniPath, "worldcam_max_hold", 24.0f);
    cfg.worldCamLOSPenalty        = ReadIniFloat(iniPath, "worldcam_los_penalty", 1000.0f);
    cfg.worldCamStickiness        = ReadIniFloat(iniPath, "worldcam_stickiness", 15.0f);

    cfg.worldCamZoomStartDist     = ReadIniFloat(iniPath, "worldcam_zoom_start_dist", 25.0f);
    cfg.worldCamZoomMaxDist       = ReadIniFloat(iniPath, "worldcam_zoom_max_dist", 50.0f);
    cfg.worldCamZoomMaxFactor     = ReadIniFloat(iniPath, "worldcam_zoom_max_factor", 0.4f);
    cfg.worldCamZoomSmoothFactor  = ReadIniFloat(iniPath, "worldcam_zoom_smooth_factor", 0.05f);

    cfg.tpvMaxDistance            = ReadIniFloat(iniPath, "tpv_max_distance", 2.5f);
    cfg.tpvYawSmoothFactor       = ReadIniFloat(iniPath, "tpv_yaw_smooth_factor", 0.01f);
    cfg.tpvZoomInFactor           = ReadIniFloat(iniPath, "tpv_zoom_in_factor", 0.3f);
    cfg.tpvZoomOutFactor          = ReadIniFloat(iniPath, "tpv_zoom_out_factor", 0.005f);

    cfg.droneSpeed                = ReadIniFloat(iniPath, "drone_speed", 8.0f);
    cfg.droneAccel                = ReadIniFloat(iniPath, "drone_accel", 12.0f);
    cfg.droneDrag                 = ReadIniFloat(iniPath, "drone_drag", 2.5f);
    cfg.droneMinWallDist          = ReadIniFloat(iniPath, "drone_min_wall_dist", 2.0f);
    cfg.droneTurnRate             = ReadIniFloat(iniPath, "drone_turn_rate", 2.0f);
    cfg.droneOrbitRadius          = ReadIniFloat(iniPath, "drone_orbit_radius", 8.0f);
    cfg.droneOrbitHeight          = ReadIniFloat(iniPath, "drone_orbit_height", 3.0f);
    cfg.droneLookSmooth           = ReadIniFloat(iniPath, "drone_look_smooth", 0.08f);
    cfg.droneHoldDuration         = ReadIniFloat(iniPath, "drone_hold_duration", 8.0f);
    cfg.droneIdleTimeout          = ReadIniFloat(iniPath, "drone_idle_timeout", 15.0f);
    cfg.camSharePlayer            = ReadIniFloat(iniPath, "cam_share_player", 0.12f);
    cfg.camShareWorld             = ReadIniFloat(iniPath, "cam_share_world", 0.80f);
    cfg.camShareDrone             = ReadIniFloat(iniPath, "cam_share_drone", 0.08f);
    cfg.camMinHoldPlayer          = ReadIniFloat(iniPath, "cam_min_hold_player", 14.0f);
    cfg.camMinHoldWorld           = ReadIniFloat(iniPath, "cam_min_hold_world", 20.0f);
    cfg.camMinHoldDrone           = ReadIniFloat(iniPath, "cam_min_hold_drone", 20.0f);
    cfg.droneMinAreaClearance     = ReadIniFloat(iniPath, "drone_min_area_clearance", 5.0f);

    cfg.droneGridResolution       = ReadIniFloat(iniPath, "drone_grid_resolution", 1.0f);
    cfg.droneClearanceBias        = ReadIniFloat(iniPath, "drone_clearance_bias", 5.0f);
    cfg.droneSlowClearance        = ReadIniFloat(iniPath, "drone_slow_clearance", 4.0f);
    cfg.dronePathReplanDist       = ReadIniFloat(iniPath, "drone_path_replan_dist", 5.0f);

    cfg.droneKillFrameDuration    = ReadIniFloat(iniPath, "drone_kill_frame_duration", 4.0f);
    cfg.droneVantageHeight        = ReadIniFloat(iniPath, "drone_vantage_height", 6.0f);
    cfg.droneVantageRadius        = ReadIniFloat(iniPath, "drone_vantage_radius", 10.0f);
    cfg.droneVantageRecomputeDist = ReadIniFloat(iniPath, "drone_vantage_recompute_dist", 5.0f);

    cfg.debugMode                 = (ReadIniInt(iniPath, "debug_mode", 0) != 0);

    std::cout << "[CameraDirector] Config loaded from " << iniPath << "\n";
}
