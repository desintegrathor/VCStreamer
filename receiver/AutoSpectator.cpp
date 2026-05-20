#define _CRT_SECURE_NO_WARNINGS
#include "AutoSpectator.h"
#include "DroneCamera.h"
#include "OctCollision.h"
#include "WorldCameraTracker.h"
#include "minhook/MinHook.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <algorithm>
#include <cctype>
#include <windows.h>

// Debug log file - written next to game exe
static FILE* g_logFile = nullptr;

static bool SafeReadDword(uintptr_t addr, uintptr_t* outVal) {
    __try {
        *outVal = *(uintptr_t*)addr;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void LogDebug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (g_logFile) {
        vfprintf(g_logFile, fmt, args);
        fflush(g_logFile);
    }
    // Also print to console
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

// Game state memory offsets (from game.dll base)
// IDA base = 0xD70000, so RVA = IDA_addr - 0xD70000
constexpr uintptr_t MP_STATE_OFFSET = 0x20DA7C;       // dword_F7DA7C - multiplayer state (2=singleplayer, 3=LAN/direct IP)
constexpr uintptr_t CONN_PTR_OFFSET = 0x7C0F4C;       // dword_1530F4C - connection pointer (always set when connected)
constexpr uintptr_t SPECT_VIEW_OBJ_OFFSET = 0x7AE320;  // dword_151E320 - STATIC spectator view object (first DWORD = camera mode)
constexpr uintptr_t SPECTATOR_CTRL_OFFSET = 0x7AECC0;  // dword_151ECC0 - spectator controller type (1=basic, 2=replay)
constexpr uintptr_t TEAM_SEL_OBJ_OFFSET = 0x7965D8;   // Global team selection object (this ptr for sub_EADA00)
constexpr uintptr_t TEAM_SEL_FUNC_OFFSET = 0x13DA00;   // sub_EADA00 - team/class selection handler
constexpr uintptr_t MP_SUBSTATE_OFFSET = 0x7C0F58;     // dword_1530F58 - MP sub-state (10 = spectator spawned)
constexpr uintptr_t SPECT_CAM_UPDATE_OFFSET = 0x148100; // sub_EB8100 - updates spectator camera mode display/state
constexpr uintptr_t SPECT_CAM_INIT_OFFSET = 0x146F90;   // sub_EB6F90 - full spectator camera initialization
constexpr uintptr_t SPECT_INPUT_OFFSET = 0x1488B0;      // sub_EB88B0 - spectator input handler (hook target)

// Hook types and state for sub_EB88B0 (spectator input handler)
// sub_EB88B0 is __thiscall: int sub_EB88B0(float* this, float deltaTime)
typedef int (__thiscall *SpectInputFunc)(float* thisPtr, float deltaTime);
static SpectInputFunc g_OrigSpectInput = nullptr;
static std::atomic<bool> g_wantCameraSwitch{ false };
static DWORD g_switchRetryStart = 0;
constexpr DWORD MAX_SWITCH_RETRY_MS = 200000; // 200 seconds
constexpr DWORD SWITCH_RETRY_INTERVAL_MS = 1000; // check every 1 second
static DWORD g_lastSwitchAttempt = 0;

// int __thiscall sub_EADA00(void* this, unsigned int a2, int a3)
// a2=9 selects spectator, a3=1
typedef int (__thiscall *TeamSelectFunc)(void* thisPtr, unsigned int selection, int a3);

// void __thiscall sub_EB8100(int* this) - updates camera mode display (player list, labels)
typedef void (__thiscall *SpectCamUpdateFunc)(int* thisPtr);

// int __thiscall sub_EB6F90(int* this) - full spectator camera init (sets mode, calls sub_EB8100)
typedef int (__thiscall *SpectCamInitFunc)(int* thisPtr);

enum class AutoState {
    WAITING_FOR_CONNECTION,
    WAITING_FOR_GAME_LOAD,
    WAITING_FOR_TEAM_MENU,
    SELECTING_SPECTATOR,
    WAITING_FOR_CONFIRMATION,
    SWITCHING_CAMERA,
    MONITORING
};

static uintptr_t g_baseGame = 0;
static std::atomic<bool> g_running{ false };
static std::thread g_autoThread;

static bool g_autoConnectEnabled = true;
static std::string g_autoConnectIp = "46.13.190.168";
static int g_autoConnectPort = 5425;
static std::string g_autoConnectAddon = "fistalpha";
static int g_autoConnectRetrySeconds = 30;

static std::string Trim(std::string value) {
    const char* ws = " \t\r\n";
    size_t first = value.find_first_not_of(ws);
    if (first == std::string::npos) return std::string();
    size_t last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
}

static bool ParseBool(std::string value, bool defaultValue) {
    value = Trim(value);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return (char)tolower(ch); });
    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
    return defaultValue;
}

static std::string GetDllDirectoryPath() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&InitAutoSpectator),
                            &self)) {
        return std::string();
    }

    char path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::string();

    std::string full(path, path + len);
    size_t slash = full.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    return full.substr(0, slash + 1);
}

static void LoadAutoConnectConfig() {
    std::ifstream ini(GetDllDirectoryPath() + "vcstreamer.ini");
    if (!ini.is_open()) return;

    std::string line;
    while (std::getline(ini, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        size_t equals = line.find('=');
        if (equals == std::string::npos) continue;

        std::string key = Trim(line.substr(0, equals));
        std::string value = Trim(line.substr(equals + 1));

        try {
            if (key == "auto_connect") {
                g_autoConnectEnabled = ParseBool(value, g_autoConnectEnabled);
            } else if (key == "server_ip") {
                g_autoConnectIp = value;
            } else if (key == "server_port") {
                g_autoConnectPort = std::stoi(value);
            } else if (key == "server_addon") {
                g_autoConnectAddon = value;
            } else if (key == "auto_connect_retry_seconds") {
                g_autoConnectRetrySeconds = std::stoi(value);
            }
        } catch (...) {
        }
    }

    if (g_autoConnectPort <= 0) g_autoConnectPort = 5425;
    if (g_autoConnectRetrySeconds < 5) g_autoConnectRetrySeconds = 5;
    if (g_autoConnectIp.empty()) g_autoConnectEnabled = false;
}

static std::string BuildAutoConnectCommandLine(const char* exePath) {
    std::string cmdLine = "\"";
    cmdLine += exePath;
    cmdLine += "\"";

    cmdLine += " -ip ";
    cmdLine += g_autoConnectIp;
    cmdLine += " ";
    cmdLine += std::to_string(g_autoConnectPort);

    if (!g_autoConnectAddon.empty()) {
        cmdLine += " -addon ";
        cmdLine += g_autoConnectAddon;
    }

    return cmdLine;
}

// Hook for sub_EB88B0 - runs on main game thread every frame during spectating.
// Uses __fastcall to handle __thiscall on x86 (thisPtr in ECX, EDX unused).
int __fastcall Hooked_SpectInput(float* thisPtr, void* /*edx*/, float deltaTime) {
    if (g_wantCameraSwitch.load()) {
        DWORD now = GetTickCount();

        // First time: initialize start time
        if (g_switchRetryStart == 0) {
            g_switchRetryStart = now;
            g_lastSwitchAttempt = 0;
        }

        DWORD elapsed = now - g_switchRetryStart;

        // Only check once per second
        if (now - g_lastSwitchAttempt >= SWITCH_RETRY_INTERVAL_MS) {
            g_lastSwitchAttempt = now;

            int* objPtr = (int*)thisPtr;
            int mode2Count = objPtr[9];  // this[9] = entry count for mode 2

            if (mode2Count > 0) {
                // Mode 2 has data — switch now
                int oldMode = *objPtr;
                *objPtr = 2;

                SpectCamUpdateFunc camUpdate = (SpectCamUpdateFunc)(g_baseGame + SPECT_CAM_UPDATE_OFFSET);
                camUpdate(objPtr);

                g_wantCameraSwitch = false;
                g_switchRetryStart = 0;
                LogDebug("[AutoSpectator] Hook: switched camera %d -> 2 (mode2_count=%d, after %dms)\n",
                         oldMode, mode2Count, elapsed);
            } else if (elapsed >= MAX_SWITCH_RETRY_MS) {
                g_wantCameraSwitch = false;
                g_switchRetryStart = 0;
                LogDebug("[AutoSpectator] Hook: gave up waiting for mode 2 data after %d seconds\n",
                         elapsed / 1000);
            } else {
                LogDebug("[AutoSpectator] Hook: waiting for mode 2 data... %ds elapsed, this[9]=%d\n",
                         elapsed / 1000, mode2Count);
            }
        }
    }
    WorldCameraTracker_Update((int*)thisPtr, g_baseGame);
    if (DroneCamera_IsActive()) {
        DroneCamera_Update(deltaTime);
    }
    return g_OrigSpectInput(thisPtr, deltaTime);
}

static void AutoSpectatorLoop() {
    AutoState state = AutoState::WAITING_FOR_CONNECTION;
    int retryCount = 0;
    bool hasConnectedOnce = false;  // once true, never restart on connection loss
    constexpr int MAX_MENU_WAIT_RETRIES = 50; // 50 * 200ms = 10 seconds
    constexpr int MAX_CONFIRM_RETRIES = 100;  // 100 * 200ms = 20 seconds

    LogDebug("[AutoSpectator] Waiting for connection...\n");

    while (g_running) {
        switch (state) {

        case AutoState::WAITING_FOR_CONNECTION: {
            uintptr_t connPtrAddr = g_baseGame + CONN_PTR_OFFSET;
            uintptr_t connPtr = *(uintptr_t*)(connPtrAddr);
            if (connPtr != 0) {
                LogDebug("[AutoSpectator] Connected! connPtr=0x%08X (at 0x%08X)\n", connPtr, connPtrAddr);
                hasConnectedOnce = true;
                state = AutoState::WAITING_FOR_GAME_LOAD;
                retryCount = 0;
            } else {
                retryCount++;
                // The -ip flag triggers connect immediately on startup. If there
                // is still no connection after the configured timeout, optionally
                // restart Vietcong with the server from vcstreamer.ini.
                int maxRetries = (g_autoConnectRetrySeconds * 1000) / 200;
                if (maxRetries < 1) maxRetries = 1;
                if (retryCount >= maxRetries && !hasConnectedOnce) {
                    if (!g_autoConnectEnabled) {
                        LogDebug("[AutoSpectator] No connection after %ds; auto_connect=0, waiting for manual connection\n",
                                 g_autoConnectRetrySeconds);
                        retryCount = 0;
                        break;
                    }

                    LogDebug("[AutoSpectator] No connection after %ds, restarting game with server %s:%d addon=%s...\n",
                             g_autoConnectRetrySeconds,
                             g_autoConnectIp.c_str(),
                             g_autoConnectPort,
                             g_autoConnectAddon.c_str());
                    char exePath[MAX_PATH];
                    GetModuleFileNameA(NULL, exePath, MAX_PATH);
                    std::string cmdLine = BuildAutoConnectCommandLine(exePath);
                    STARTUPINFOA si = { sizeof(si) };
                    PROCESS_INFORMATION pi;
                    if (CreateProcessA(NULL, &cmdLine[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                        LogDebug("[AutoSpectator] New process started, exiting current...\n");
                        if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
                        ExitProcess(0);
                    } else {
                        LogDebug("[AutoSpectator] Failed to restart (err=%d), retrying...\n", GetLastError());
                        retryCount = 0;
                    }
                }
            }
            break;
        }

        case AutoState::WAITING_FOR_GAME_LOAD: {
            int mpState = *(int*)(g_baseGame + MP_STATE_OFFSET);
            // mpState: 1=IPX, 2=singleplayer, 3=LAN/direct-IP. Any non-zero means active.
            if (mpState >= 1) {
                LogDebug("[AutoSpectator] Game loaded! mpState=%d\n", mpState);
                state = AutoState::WAITING_FOR_TEAM_MENU;
                retryCount = 0;
            } else {
                // Log every 10 ticks
                if (retryCount++ % 10 == 0)
                    LogDebug("[AutoSpectator] Waiting for game load... mpState=%d\n", mpState);
            }
            // If connection drops, go back
            uintptr_t connPtr = *(uintptr_t*)(g_baseGame + CONN_PTR_OFFSET);
            if (connPtr == 0) {
                LogDebug("[AutoSpectator] Connection lost during game load wait\n");
                state = AutoState::WAITING_FOR_CONNECTION;
                retryCount = 0;
            }
            break;
        }

        case AutoState::WAITING_FOR_TEAM_MENU: {
            // TEAM_SEL_OBJ_OFFSET is a direct global object (not a pointer to one)
            // The first DWORD is the menu state: 1=team select, 2=class select
            uintptr_t teamSelObjAddr = g_baseGame + TEAM_SEL_OBJ_OFFSET;
            int menuState = *(int*)(teamSelObjAddr);

            if (retryCount % 5 == 0) {
                LogDebug("[AutoSpectator] Team sel obj at 0x%08X, menuState=%d\n",
                         teamSelObjAddr, menuState);
            }

            if (menuState == 1 || menuState == 2) {
                LogDebug("[AutoSpectator] Team selection menu active (state=%d), joining as spectator...\n", menuState);
                state = AutoState::SELECTING_SPECTATOR;
            } else {
                retryCount++;
                if (retryCount % 5 == 0) {
                    LogDebug("[AutoSpectator] Waiting for team menu... (menuState=%d, attempt %d/%d)\n",
                             menuState, retryCount, MAX_MENU_WAIT_RETRIES);
                }
                if (retryCount >= MAX_MENU_WAIT_RETRIES) {
                    LogDebug("[AutoSpectator] Team menu didn't appear after %d attempts, retrying\n", MAX_MENU_WAIT_RETRIES);
                    state = AutoState::WAITING_FOR_GAME_LOAD;
                    retryCount = 0;
                }
            }

            // If connection drops, go back
            uintptr_t connPtr = *(uintptr_t*)(g_baseGame + CONN_PTR_OFFSET);
            if (connPtr == 0) {
                LogDebug("[AutoSpectator] Connection lost during team menu wait\n");
                state = AutoState::WAITING_FOR_CONNECTION;
                retryCount = 0;
            }
            break;
        }

        case AutoState::SELECTING_SPECTATOR: {
            // TEAM_SEL_OBJ_OFFSET is the direct object address (this ptr for sub_EADA00)
            void* teamSelObj = (void*)(g_baseGame + TEAM_SEL_OBJ_OFFSET);
            TeamSelectFunc teamSelectFunc = (TeamSelectFunc)(g_baseGame + TEAM_SEL_FUNC_OFFSET);

            LogDebug("[AutoSpectator] Calling team select: func=0x%08X, thisPtr=0x%08X, args=(9, 1)\n",
                     (uintptr_t)teamSelectFunc, (uintptr_t)teamSelObj);

            int result = teamSelectFunc(teamSelObj, 9, 1);

            LogDebug("[AutoSpectator] Team select returned %d, waiting for confirmation...\n", result);
            state = AutoState::WAITING_FOR_CONFIRMATION;
            retryCount = 0;
            break;
        }

        case AutoState::WAITING_FOR_CONFIRMATION: {
            // dword_151E320 is a STATIC object, not a pointer. Its first DWORD is camera mode (0=player follow).
            // We confirm spectator spawn by checking mpSubState == 10 (set by sub_ED9F90).
            int mpSubState = *(int*)(g_baseGame + MP_SUBSTATE_OFFSET);
            int spectCtrl = *(int*)(g_baseGame + SPECTATOR_CTRL_OFFSET);

            if (mpSubState == 10) {
                LogDebug("[AutoSpectator] Spectator spawned! mpSubState=%d, spectCtrl=%d\n",
                         mpSubState, spectCtrl);
                OctCollision_LoadCurrentMap();
                state = AutoState::SWITCHING_CAMERA;
                retryCount = 0;
            } else {
                retryCount++;
                if (retryCount % 5 == 0) {
                    LogDebug("[AutoSpectator] Waiting for spectator spawn... mpSubState=%d, spectCtrl=%d, attempt %d/%d\n",
                             mpSubState, spectCtrl, retryCount, MAX_CONFIRM_RETRIES);
                }
                if (retryCount >= MAX_CONFIRM_RETRIES) {
                    LogDebug("[AutoSpectator] Not spawned after %d attempts, retrying selection\n", MAX_CONFIRM_RETRIES);
                    state = AutoState::WAITING_FOR_TEAM_MENU;
                    retryCount = 0;
                }
            }

            // If connection drops, go back
            uintptr_t connPtr = *(uintptr_t*)(g_baseGame + CONN_PTR_OFFSET);
            if (connPtr == 0) {
                LogDebug("[AutoSpectator] Connection lost during confirmation wait\n");
                state = AutoState::WAITING_FOR_CONNECTION;
                retryCount = 0;
            }
            break;
        }

        case AutoState::SWITCHING_CAMERA: {
            // Wait a bit for spectator view to fully initialize (1 second after spawn)
            if (retryCount < 5) {
                retryCount++;
                break;
            }

            // Set the flag — the hook on sub_EB88B0 (main thread) will do the actual switch
            g_wantCameraSwitch = true;
            LogDebug("[AutoSpectator] Requested camera switch via hook\n");
            state = AutoState::MONITORING;
            break;
        }

        case AutoState::MONITORING: {
            int mpSubState = *(int*)(g_baseGame + MP_SUBSTATE_OFFSET);
            uintptr_t connPtr = *(uintptr_t*)(g_baseGame + CONN_PTR_OFFSET);

            if (connPtr == 0) {
                LogDebug("[AutoSpectator] Connection lost during monitoring\n");
                state = AutoState::WAITING_FOR_CONNECTION;
                retryCount = 0;
            } else if (mpSubState != 10) {
                LogDebug("[AutoSpectator] Map change detected (mpSubState=%d), re-selecting spectator...\n", mpSubState);
                state = AutoState::WAITING_FOR_TEAM_MENU;
                retryCount = 0;
            }
            break;
        }
        }

        Sleep(200);
    }

    if (!g_running) {
        LogDebug("[AutoSpectator] Thread stopped (shutdown requested)\n");
    }
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void InitAutoSpectator(uintptr_t baseGame) {
    g_baseGame = baseGame;

    // Open debug log file next to the game exe
    g_logFile = fopen("autospectator_debug.log", "w");

    LogDebug("[AutoSpectator] Initializing auto-spectator...\n");
    LogDebug("[AutoSpectator] game.dll base: 0x%08X\n", baseGame);
    LogDebug("[AutoSpectator] Connection ptr addr: 0x%08X (offset 0x%X)\n", baseGame + CONN_PTR_OFFSET, CONN_PTR_OFFSET);
    LogDebug("[AutoSpectator] MP state addr:      0x%08X (offset 0x%X)\n", baseGame + MP_STATE_OFFSET, MP_STATE_OFFSET);
    LogDebug("[AutoSpectator] Team sel obj addr:  0x%08X (offset 0x%X)\n", baseGame + TEAM_SEL_OBJ_OFFSET, TEAM_SEL_OBJ_OFFSET);
    LogDebug("[AutoSpectator] Team sel func addr: 0x%08X (offset 0x%X)\n", baseGame + TEAM_SEL_FUNC_OFFSET, TEAM_SEL_FUNC_OFFSET);
    LogDebug("[AutoSpectator] MP substate addr:   0x%08X (offset 0x%X)\n", baseGame + MP_SUBSTATE_OFFSET, MP_SUBSTATE_OFFSET);
    LogDebug("[AutoSpectator] Spectator ctrl addr:0x%08X (offset 0x%X)\n", baseGame + SPECTATOR_CTRL_OFFSET, SPECTATOR_CTRL_OFFSET);
    LogDebug("[AutoSpectator] Spect view obj addr:0x%08X (offset 0x%X)\n", baseGame + SPECT_VIEW_OBJ_OFFSET, SPECT_VIEW_OBJ_OFFSET);
    LogDebug("[AutoSpectator] CamInit func addr:  0x%08X (offset 0x%X)\n", baseGame + SPECT_CAM_INIT_OFFSET, SPECT_CAM_INIT_OFFSET);
    LogDebug("[AutoSpectator] CamUpdate func addr:0x%08X (offset 0x%X)\n", baseGame + SPECT_CAM_UPDATE_OFFSET, SPECT_CAM_UPDATE_OFFSET);

    LoadAutoConnectConfig();
    LogDebug("[AutoSpectator] Auto-connect: enabled=%d server=%s:%d addon=%s retry=%ds\n",
             g_autoConnectEnabled ? 1 : 0,
             g_autoConnectIp.c_str(),
             g_autoConnectPort,
             g_autoConnectAddon.c_str(),
             g_autoConnectRetrySeconds);

    OctCollision_Init(baseGame);

    // Install hook on sub_EB88B0 (spectator input handler) for main-thread camera switching.
    // MinHook is already initialized by FirstPersonCamera.
    uintptr_t spectInputAddr = baseGame + SPECT_INPUT_OFFSET;
    MH_STATUS mhStatus = MH_CreateHook((LPVOID)spectInputAddr, &Hooked_SpectInput, (LPVOID*)&g_OrigSpectInput);
    if (mhStatus == MH_OK) {
        mhStatus = MH_EnableHook((LPVOID)spectInputAddr);
        if (mhStatus == MH_OK) {
            LogDebug("[AutoSpectator] Hook installed on sub_EB88B0 at 0x%08X\n", spectInputAddr);
        } else {
            LogDebug("[AutoSpectator] Failed to enable hook: %s\n", MH_StatusToString(mhStatus));
        }
    } else {
        LogDebug("[AutoSpectator] Failed to create hook: %s\n", MH_StatusToString(mhStatus));
    }

    g_running = true;
    g_autoThread = std::thread(AutoSpectatorLoop);
    g_autoThread.detach();
}

void ShutdownAutoSpectator() {
    g_running = false;

    // Disable our hook (don't MH_Uninitialize — FirstPersonCamera also uses MinHook)
    if (g_OrigSpectInput) {
        uintptr_t spectInputAddr = g_baseGame + SPECT_INPUT_OFFSET;
        MH_DisableHook((LPVOID)spectInputAddr);
    }

    LogDebug("[AutoSpectator] Shutdown requested\n");
}
