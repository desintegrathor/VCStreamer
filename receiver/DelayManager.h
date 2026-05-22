#pragma once
#include <Windows.h>

class DelayManager {
public:
    static void Init();
    static int GetGameDelaySeconds();
    static int GetMinimumGameDelaySeconds() { return minGameDelaySeconds; }
    static void EnsureMinimumGameDelaySeconds();
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
    // FPV camera offsets
    static float fpvOffsetBack;
    static float fpvOffsetLeft;
    static float fpvOffsetUp;
    static float fpvPitchOffset;
    static int fpvChance;
    static bool debugMode;
    static int minGameDelaySeconds;
    static DWORD lastMinDelayLog;

    // Config reload timing
    static DWORD lastConfigReload;
    static const DWORD CONFIG_RELOAD_INTERVAL = 5000;  // 5 seconds

    static uintptr_t gameBase;
    static int LoadDelayFromINI();
};
