#define _CRT_SECURE_NO_WARNINGS
#include "PlayerIdentityCache.h"
#include "DiagnosticsLog.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

constexpr uintptr_t kPlayerTableOffset = 0x7AE9C8;
constexpr DWORD kPollIntervalMs = 1000;

uintptr_t g_gameBase = 0;
DWORD g_lastPoll = 0;
std::mutex g_mutex;
std::unordered_map<int, PlayerIdentity> g_players;

bool IsDigitString(const char* text) {
    if (!text || !text[0]) return false;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

bool StartsWith(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

void CopyCleanName(const char* raw, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!raw) return;

    size_t w = 0;
    for (size_t r = 0; raw[r] != '\0' && w + 1 < outLen && r < 63; ++r) {
        unsigned char ch = static_cast<unsigned char>(raw[r]);
        if (ch < 32 || ch >= 127) {
            continue;
        }
        out[w++] = static_cast<char>(ch);
    }
    out[w] = '\0';

    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) {
        out[--w] = '\0';
    }
}

bool ReadPlayerTable(uintptr_t gameBase, PlayerIdentity* outPlayers, int* outCount) {
    if (!outPlayers || !outCount) return false;
    *outCount = 0;

    __try {
        void** playerTable = reinterpret_cast<void**>(gameBase + kPlayerTableOffset);
        for (int i = 0; i < 64; ++i) {
            void* entry = playerTable[i];
            if (!entry) continue;

            PlayerIdentity identity = {};
            identity.handle = *reinterpret_cast<int*>(entry);
            identity.team = *(reinterpret_cast<int*>(entry) + 5);
            CopyCleanName(reinterpret_cast<const char*>(entry) + 40,
                          identity.name,
                          sizeof(identity.name));

            if (identity.handle <= 0) {
                continue;
            }
            if (!PlayerIdentityCache_IsResolvedName(identity.name)) {
                identity.name[0] = '\0';
            }
            outPlayers[*outCount] = identity;
            ++(*outCount);
            if (*outCount >= 64) break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

} // namespace

bool PlayerIdentityCache_IsResolvedName(const char* name) {
    if (!name || !name[0]) return false;
    while (*name == ' ' || *name == '\t') ++name;
    if (!name[0]) return false;

    if (StartsWith(name, "Spectator")) return false;
    if (StartsWith(name, "Player ")) {
        return !IsDigitString(name + 7);
    }
    return true;
}

void PlayerIdentityCache_Init(uintptr_t gameBase) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gameBase = gameBase;
    g_lastPoll = 0;
    g_players.clear();
    DiagnosticsLog_Append("commentary_debug.log",
                          "[Identity] initialized gameBase=0x%08X\n",
                          (unsigned)g_gameBase);
}

void PlayerIdentityCache_Poll() {
    DWORD now = GetTickCount();
    uintptr_t gameBase = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        gameBase = g_gameBase;
        if (gameBase == 0 || (g_lastPoll != 0 && now - g_lastPoll < kPollIntervalMs)) {
            return;
        }
    }

    PlayerIdentity entries[64] = {};
    int count = 0;
    if (!ReadPlayerTable(gameBase, entries, &count)) {
        DiagnosticsLog_Append("commentary_debug.log", "[Identity] poll exception\n");
        return;
    }

    std::unordered_map<int, PlayerIdentity> next;
    for (int i = 0; i < count; ++i) {
        next[entries[i].handle] = entries[i];
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_players.swap(next);
    g_lastPoll = now;
}

bool PlayerIdentityCache_Resolve(int handle, PlayerIdentity* out) {
    if (handle <= 0 || !out) return false;
    PlayerIdentityCache_Poll();

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_players.find(handle);
    if (it == g_players.end()) {
        return false;
    }
    *out = it->second;
    return true;
}
