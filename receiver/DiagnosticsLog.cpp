#define _CRT_SECURE_NO_WARNINGS
#include "DiagnosticsLog.h"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <strsafe.h>

static std::atomic<bool> g_diagnosticsEnabled{ false };
static std::mutex g_appendLogMutex;

void DiagnosticsLog_SetEnabled(bool enabled) {
    g_diagnosticsEnabled.store(enabled);
}

bool DiagnosticsLog_IsEnabled() {
    return g_diagnosticsEnabled.load();
}

void DiagnosticsLog_Write(FILE* file, const char* fmt, va_list args) {
    if (!DiagnosticsLog_IsEnabled()) return;
    if (!file || !fmt) return;

    char line[4096];
    HRESULT hr = StringCchVPrintfA(line, ARRAYSIZE(line), fmt, args);
    if (FAILED(hr) && hr != STRSAFE_E_INSUFFICIENT_BUFFER) {
        return;
    }

    line[ARRAYSIZE(line) - 1] = '\0';
    size_t len = strlen(line);
    if (len > 0) {
        fwrite(line, 1, len, file);
        fflush(file);
    }
}

void DiagnosticsLog_Append(const char* fileName, const char* fmt, ...) {
    if (!DiagnosticsLog_IsEnabled()) return;
    if (!fileName || !fmt) return;

    std::lock_guard<std::mutex> lock(g_appendLogMutex);
    FILE* file = fopen(fileName, "a");
    if (!file) return;

    va_list args;
    va_start(args, fmt);
    DiagnosticsLog_Write(file, fmt, args);
    va_end(args);

    fclose(file);
}
