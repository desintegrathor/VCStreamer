#include "YoutubeChatBridge.h"
#include "DiagnosticsLog.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr const char* PIPE_NAME = R"(\\.\pipe\vcstreamer_youtube_chat)";
constexpr size_t DEFAULT_QUEUE_LIMIT = 50;
constexpr int DEFAULT_SEND_INTERVAL_MS = 1500;
constexpr int DEFAULT_MAX_CHARS = 480;
constexpr size_t MAX_AUTHOR_CHARS = 64;
constexpr size_t MAX_MESSAGE_CHARS = 480;

struct YoutubeChatConfig {
    bool enabled = false;
    int minSendIntervalMs = DEFAULT_SEND_INTERVAL_MS;
    size_t queueLimit = DEFAULT_QUEUE_LIMIT;
    int maxChars = DEFAULT_MAX_CHARS;
};

struct YoutubeChatItem {
    std::string author;
    std::string message;
};

using VchdStreamerIsReadyFn = int(__cdecl*)();
using VchdStreamerSubmitYoutubeChatFn = int(__cdecl*)(const char*, const char*);

struct VchdStreamerApi {
    HMODULE module = nullptr;
    VchdStreamerIsReadyFn isReady = nullptr;
    VchdStreamerSubmitYoutubeChatFn submitYoutubeChat = nullptr;
};

std::atomic<bool> g_started{ false };
YoutubeChatConfig g_config;
std::mutex g_queueMutex;
std::deque<YoutubeChatItem> g_queue;
VchdStreamerApi g_vchdApi;

std::string GetDllDirectoryPath() {
    char dllPath[MAX_PATH] = {};
    HMODULE hModule = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&GetDllDirectoryPath,
                           &hModule)) {
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);
        std::string fullPath(dllPath);
        size_t lastSlash = fullPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            return fullPath.substr(0, lastSlash + 1);
        }
    }
    return std::string();
}

int ReadInt(const std::string& iniPath, const char* key, int defaultValue) {
    return GetPrivateProfileIntA("YouTubeChat", key, defaultValue, iniPath.c_str());
}

YoutubeChatConfig LoadConfig() {
    YoutubeChatConfig cfg;
    const std::string iniPath = GetDllDirectoryPath() + "vcstreamer.ini";
    cfg.enabled = ReadInt(iniPath, "enabled", 0) != 0;
    cfg.minSendIntervalMs = (std::max)(250, ReadInt(iniPath, "min_send_interval_ms", DEFAULT_SEND_INTERVAL_MS));
    cfg.queueLimit = (size_t)(std::max)(1, ReadInt(iniPath, "queue_limit", (int)DEFAULT_QUEUE_LIMIT));
    cfg.maxChars = (std::max)(1, (std::min)((int)MAX_MESSAGE_CHARS, ReadInt(iniPath, "max_chars", DEFAULT_MAX_CHARS)));
    return cfg;
}

void BridgeLog(const char* fmt, ...) {
    const std::string logPath = GetDllDirectoryPath() + "youtube_chat_bridge.log";
    FILE* file = nullptr;
    if (fopen_s(&file, logPath.c_str(), "a") != 0) {
        file = nullptr;
    }
    if (!file) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fclose(file);
}

std::string TrimAndCollapseSpaces(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    bool prevSpace = true;

    for (char ch : value) {
        unsigned char uch = (unsigned char)ch;
        if (uch == '\r' || uch == '\n' || uch == '\t') {
            uch = ' ';
        }
        if (uch < 32 || uch > 126) {
            continue;
        }
        if (uch == ' ') {
            if (!prevSpace) {
                out.push_back(' ');
            }
            prevSpace = true;
        } else {
            out.push_back((char)uch);
            prevSpace = false;
        }
    }

    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.size() > (size_t)g_config.maxChars) {
        out.resize((size_t)g_config.maxChars);
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
    }
    return out;
}

std::string TrimAuthor(const std::string& value) {
    std::string author = TrimAndCollapseSpaces(value);
    while (!author.empty() && author.front() == ' ') {
        author.erase(author.begin());
    }
    if (!author.empty() && author.front() == '@') {
        author.erase(author.begin());
    }
    while (!author.empty() && author.front() == ' ') {
        author.erase(author.begin());
    }
    if (author.size() > MAX_AUTHOR_CHARS) {
        author.resize(MAX_AUTHOR_CHARS);
        while (!author.empty() && author.back() == ' ') {
            author.pop_back();
        }
    }
    if (author.empty()) {
        author = "viewer";
    }
    return author;
}

YoutubeChatItem ExtractChatItem(const std::string& line) {
    YoutubeChatItem item;
    try {
        auto json = nlohmann::json::parse(line);
        if (json.is_object()) {
            if (json.contains("author") && json["author"].is_string()) {
                item.author = json["author"].get<std::string>();
            }
            if (json.contains("message") && json["message"].is_string()) {
                item.message = json["message"].get<std::string>();
            } else if (json.contains("text") && json["text"].is_string()) {
                item.message = json["text"].get<std::string>();
            }
        }
    } catch (...) {
        item.message = line;
    }

    item.author = TrimAuthor(item.author);
    item.message = TrimAndCollapseSpaces(item.message);
    return item;
}

void EnqueueItem(const YoutubeChatItem& item) {
    if (item.message.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_queueMutex);
    while (g_queue.size() >= g_config.queueLimit) {
        g_queue.pop_front();
    }
    g_queue.push_back(item);
}

bool PopItem(YoutubeChatItem& item) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.empty()) {
        return false;
    }
    item = g_queue.front();
    g_queue.pop_front();
    return true;
}

bool ResolveVchdStreamerApi() {
    HMODULE module = GetModuleHandleA("dinput8.dll");
    if (!module) {
        module = GetModuleHandleA("vchd_dinput8.dll");
    }
    if (!module) {
        return false;
    }

    if (g_vchdApi.module == module && g_vchdApi.isReady && g_vchdApi.submitYoutubeChat) {
        return true;
    }

    g_vchdApi.module = module;
    g_vchdApi.isReady = reinterpret_cast<VchdStreamerIsReadyFn>(
        GetProcAddress(module, "VCHD_StreamerIsReady"));
    g_vchdApi.submitYoutubeChat = reinterpret_cast<VchdStreamerSubmitYoutubeChatFn>(
        GetProcAddress(module, "VCHD_StreamerSubmitYoutubeChat"));
    if (!g_vchdApi.isReady || !g_vchdApi.submitYoutubeChat) {
        g_vchdApi = VchdStreamerApi{};
        return false;
    }
    return true;
}

void HandlePipeClient(HANDLE pipe) {
    char buffer[1024];
    std::string pending;

    for (;;) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr);
        if (!ok || bytesRead == 0) {
            break;
        }

        pending.append(buffer, buffer + bytesRead);
        for (;;) {
            size_t newline = pending.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            EnqueueItem(ExtractChatItem(line));
        }

        if (pending.size() > 4096) {
            pending.clear();
        }
    }

    EnqueueItem(ExtractChatItem(pending));
}

void PipeServerThread() {
    BridgeLog("[YouTubeChat] Pipe server starting: %s\n", PIPE_NAME);

    for (;;) {
        HANDLE pipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            BridgeLog("[YouTubeChat] CreateNamedPipe failed: %lu\n", GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            BridgeLog("[YouTubeChat] Helper connected\n");
            HandlePipeClient(pipe);
            BridgeLog("[YouTubeChat] Helper disconnected\n");
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void SenderThread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_config.minSendIntervalMs));

        YoutubeChatItem item;
        if (!PopItem(item)) {
            continue;
        }

        if (!ResolveVchdStreamerApi() || !g_vchdApi.isReady()) {
            EnqueueItem(item);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        if (g_vchdApi.submitYoutubeChat(item.author.c_str(), item.message.c_str())) {
            BridgeLog("[YouTubeChat] Submitted via VCHD: %s: %s\n",
                      item.author.c_str(),
                      item.message.c_str());
        } else {
            BridgeLog("[YouTubeChat] VCHD submit failed, requeue: %s: %s\n",
                      item.author.c_str(),
                      item.message.c_str());
            EnqueueItem(item);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
}

}

void InitYoutubeChatBridge(uintptr_t gameBase) {
    if (g_started.exchange(true)) {
        return;
    }

    (void)gameBase;
    g_config = LoadConfig();
    if (!g_config.enabled) {
        return;
    }

    BridgeLog("[YouTubeChat] Enabled. interval=%dms queue=%u max_chars=%d transport=VCHD\n",
              g_config.minSendIntervalMs,
              (unsigned)g_config.queueLimit,
              g_config.maxChars);

    std::thread(PipeServerThread).detach();
    std::thread(SenderThread).detach();
}
