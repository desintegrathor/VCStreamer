#define _CRT_SECURE_NO_WARNINGS
#include "CommentatorFeed.h"
#include <Windows.h>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <strsafe.h>

static std::mutex g_commentatorMutex;

void CommentatorFeed_Init() {
    SetConsoleTitleA("VCStreamer Commentator Feed");
}

void CommentatorFeed_Line(const char* fmt, ...) {
    if (!fmt) return;

    char line[1024];
    va_list args;
    va_start(args, fmt);
    HRESULT hr = StringCchVPrintfA(line, ARRAYSIZE(line), fmt, args);
    va_end(args);

    if (FAILED(hr) && hr != STRSAFE_E_INSUFFICIENT_BUFFER) {
        return;
    }

    line[ARRAYSIZE(line) - 1] = '\0';
    size_t len = strlen(line);
    if (len == 0) return;

    char out[1100];
    StringCchCopyA(out, ARRAYSIZE(out), line);
    if (len >= ARRAYSIZE(out) - 2) {
        len = ARRAYSIZE(out) - 2;
        out[len] = '\0';
    }
    if (out[len - 1] != '\n') {
        out[len++] = '\n';
        out[len] = '\0';
    }

    std::lock_guard<std::mutex> lock(g_commentatorMutex);
    DWORD written = 0;
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    if (console && console != INVALID_HANDLE_VALUE) {
        WriteFile(console, out, (DWORD)len, &written, nullptr);
    }
}
