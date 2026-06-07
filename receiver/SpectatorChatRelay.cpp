#define _CRT_SECURE_NO_WARNINGS
#include "SpectatorChatRelay.h"

#include "DiagnosticsLog.h"
#include "minhook/MinHook.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

// IDA base is 0xD70000. These RVAs target game.dll v1.60.
constexpr uintptr_t RVA_GNET_CHAT_HANDLER = 0x163990;       // IDA 0xED3990
constexpr uintptr_t RVA_GNET_PROCESS_CHAT = 0x163BA0;       // IDA 0xED3BA0
constexpr uintptr_t RVA_GNET_SEND_MESSAGE = 0x14B800;       // IDA 0xEBB800
constexpr uintptr_t RVA_G_PLAYER_ARRAY = 0x7AE9C8;          // IDA 0x151E9C8
constexpr uintptr_t RVA_G_DP_SERVER_HANDLE = 0x20DA80;      // IDA 0xF7DA80
constexpr uintptr_t RVA_G_IS_SERVER = 0x7BDDF4;             // IDA 0x152DDF4

constexpr unsigned int CHAT_PACKET_TYPE = 0x23;
constexpr size_t PLAYER_COUNT = 64;
constexpr size_t PLAYER_CONNECTION_HANDLE_OFFSET = 0x00;
constexpr size_t PLAYER_STATE_OFFSET = 0x10;
constexpr size_t PLAYER_ENTITY_OFFSET = 0xF4;
constexpr size_t MAX_NATIVE_CHAT_CHARS = 1023;

using ChatHandlerFn = unsigned int(__cdecl*)(void* senderHandle, int packet);
using GnetSendMessageFn = void(__cdecl*)(void* targetHandle, char* data, unsigned int size, int reliable, float delaySeconds);
using GnetProcessChatFn = unsigned int(__cdecl*)(int packet, int readOnly);

struct IncomingChat {
    std::string rawText;
    std::string trimmedText;
    unsigned int consumedBytes = 0;
};

uintptr_t g_gameBase = 0;
void** g_playerArray = nullptr;
ChatHandlerFn g_originalChatHandler = nullptr;
GnetSendMessageFn g_sendMessage = nullptr;
GnetProcessChatFn g_processChat = nullptr;

bool SafeReadByte(uintptr_t address, unsigned char& value) {
    __try {
        value = *(unsigned char*)address;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

bool SafeReadPtr(uintptr_t address, void*& value) {
    __try {
        value = *(void**)address;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = nullptr;
        return false;
    }
}

bool SafeReadInt(uintptr_t address, int& value) {
    __try {
        value = *(int*)address;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

bool SafeSendMessage(void* targetHandle, unsigned char* data, unsigned int size) {
    __try {
        g_sendMessage(targetHandle, (char*)data, size, 2, 0.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeProcessChat(unsigned char* data) {
    __try {
        g_processChat((int)data, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

unsigned int ReadBitsFromPacket(int packet, unsigned int bitPos, unsigned int bitCount, bool& ok) {
    unsigned int value = 0;
    ok = true;

    for (unsigned int i = 0; i < bitCount; ++i) {
        unsigned char byteValue = 0;
        uintptr_t byteAddress = (uintptr_t)packet + ((bitPos + i) >> 3);
        if (!SafeReadByte(byteAddress, byteValue)) {
            ok = false;
            return 0;
        }
        if ((byteValue >> ((bitPos + i) & 7)) & 1u) {
            value |= (1u << i);
        }
    }

    return value;
}

void WriteBits(std::vector<unsigned char>& buffer, unsigned int& bitPos, unsigned int value, unsigned int bitCount) {
    for (unsigned int i = 0; i < bitCount; ++i) {
        if ((value >> i) & 1u) {
            buffer[(size_t)bitPos >> 3] |= (unsigned char)(1u << (bitPos & 7));
        }
        ++bitPos;
    }
}

std::string TrimForCommandDetection(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && std::isspace((unsigned char)text[first])) {
        ++first;
    }

    size_t last = text.size();
    while (last > first && std::isspace((unsigned char)text[last - 1])) {
        --last;
    }

    return text.substr(first, last - first);
}

bool ParseIncomingChat(int packet, IncomingChat& chat) {
    unsigned int bitPos = 8;
    bool ok = false;

    // Client-to-server chat packets contain the team/public bit after the type.
    // Spectator relay ignores it later, but reading it keeps the byte count exact.
    (void)ReadBitsFromPacket(packet, bitPos, 1, ok);
    if (!ok) {
        return false;
    }
    bitPos += 1;

    chat.rawText.clear();
    chat.rawText.reserve(80);

    for (size_t i = 0; i < MAX_NATIVE_CHAT_CHARS; ++i) {
        char ch = (char)(ReadBitsFromPacket(packet, bitPos, 7, ok) & 0x7F);
        if (!ok) {
            return false;
        }
        bitPos += 7;

        if (ch == '\0') {
            chat.consumedBytes = (bitPos + 7) >> 3;
            chat.trimmedText = TrimForCommandDetection(chat.rawText);
            return true;
        }

        chat.rawText.push_back(ch);
    }

    return false;
}

std::vector<unsigned char> BuildServerChatPacket(uintptr_t senderHandle, const std::string& text) {
    std::vector<unsigned char> packet(1024, 0);
    unsigned int bitPos = 0;

    WriteBits(packet, bitPos, CHAT_PACKET_TYPE, 8);
    WriteBits(packet, bitPos, (unsigned int)senderHandle & 0x1FFFu, 13);
    WriteBits(packet, bitPos, 0, 1); // public chat: relay spectator messages to everyone

    for (char rawCh : text) {
        WriteBits(packet, bitPos, (unsigned char)rawCh & 0x7Fu, 7);
    }
    WriteBits(packet, bitPos, 0, 7);

    packet.resize(((size_t)bitPos + 7) >> 3);
    return packet;
}

void* FindPlayerByConnectionHandle(void* senderHandle) {
    if (!g_playerArray || !senderHandle) {
        return nullptr;
    }

    for (size_t i = 0; i < PLAYER_COUNT; ++i) {
        void* player = nullptr;
        if (!SafeReadPtr((uintptr_t)&g_playerArray[i], player) || !player) {
            continue;
        }

        void* connectionHandle = nullptr;
        if (SafeReadPtr((uintptr_t)player + PLAYER_CONNECTION_HANDLE_OFFSET, connectionHandle)
            && connectionHandle == senderHandle) {
            return player;
        }
    }

    return nullptr;
}

bool IsSpectatorOrDummy(void* player) {
    if (!player) {
        return false;
    }

    void* entity = nullptr;
    int playerState = 0;

    if (!SafeReadPtr((uintptr_t)player + PLAYER_ENTITY_OFFSET, entity)) {
        return false;
    }
    if (!SafeReadInt((uintptr_t)player + PLAYER_STATE_OFFSET, playerState)) {
        return false;
    }

    return entity == nullptr || playerState == 2;
}

void RelayToAllConnectedPlayers(std::vector<unsigned char>& packet) {
    if (!g_playerArray || !g_sendMessage || packet.empty()) {
        return;
    }

    for (size_t i = 0; i < PLAYER_COUNT; ++i) {
        void* player = nullptr;
        if (!SafeReadPtr((uintptr_t)&g_playerArray[i], player) || !player) {
            continue;
        }

        void* connectionHandle = nullptr;
        if (!SafeReadPtr((uintptr_t)player + PLAYER_CONNECTION_HANDLE_OFFSET, connectionHandle) || !connectionHandle) {
            continue;
        }

        SafeSendMessage(connectionHandle, packet.data(), (unsigned int)packet.size());
    }
}

void ProcessChatForServerCapture(std::vector<unsigned char>& packet) {
    if (!g_processChat || packet.empty()) {
        return;
    }

    void* serverHandle = nullptr;
    int isServer = 0;
    if (!SafeReadPtr(g_gameBase + RVA_G_DP_SERVER_HANDLE, serverHandle) || !serverHandle) {
        return;
    }
    if (!SafeReadInt(g_gameBase + RVA_G_IS_SERVER, isServer) || !isServer) {
        return;
    }

    SafeProcessChat(packet.data());
}

unsigned int __cdecl HookedChatHandler(void* senderHandle, int packet) {
    IncomingChat chat;
    if (!ParseIncomingChat(packet, chat)) {
        return g_originalChatHandler(senderHandle, packet);
    }

    void* senderPlayer = FindPlayerByConnectionHandle(senderHandle);
    if (!senderPlayer) {
        return g_originalChatHandler(senderHandle, packet);
    }

    if (!IsSpectatorOrDummy(senderPlayer)) {
        return g_originalChatHandler(senderHandle, packet);
    }

    std::vector<unsigned char> outbound = BuildServerChatPacket((uintptr_t)senderHandle, chat.rawText);
    RelayToAllConnectedPlayers(outbound);
    ProcessChatForServerCapture(outbound);

    DiagnosticsLog_Append("receiver_debug.log",
                          "[SpectatorChatRelay] Relayed spectator chat from handle=0x%08X bytes=%u text=\"%s\"\n",
                          (unsigned)(uintptr_t)senderHandle,
                          chat.consumedBytes,
                          chat.rawText.c_str());

    return chat.consumedBytes;
}

} // namespace

bool InitSpectatorChatRelay(uintptr_t gameBase) {
    if (!gameBase) {
        return false;
    }

    g_gameBase = gameBase;
    g_playerArray = (void**)(gameBase + RVA_G_PLAYER_ARRAY);
    g_sendMessage = (GnetSendMessageFn)(gameBase + RVA_GNET_SEND_MESSAGE);
    g_processChat = (GnetProcessChatFn)(gameBase + RVA_GNET_PROCESS_CHAT);

    void* target = (void*)(gameBase + RVA_GNET_CHAT_HANDLER);
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        DiagnosticsLog_Append("receiver_debug.log",
                              "[SpectatorChatRelay] MinHook init failed: %s\n",
                              MH_StatusToString(status));
        return false;
    }

    status = MH_CreateHook(target, (void*)&HookedChatHandler, (void**)&g_originalChatHandler);
    if (status != MH_OK) {
        DiagnosticsLog_Append("receiver_debug.log",
                              "[SpectatorChatRelay] Hook create failed: %s\n",
                              MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        DiagnosticsLog_Append("receiver_debug.log",
                              "[SpectatorChatRelay] Hook enable failed: %s\n",
                              MH_StatusToString(status));
        return false;
    }

    DiagnosticsLog_Append("receiver_debug.log",
                          "[SpectatorChatRelay] Hooked game.dll+0x%X for spectator chat relay\n",
                          (unsigned)RVA_GNET_CHAT_HANDLER);
    return true;
}

void ShutdownSpectatorChatRelay() {
    if (g_gameBase) {
        MH_DisableHook((void*)(g_gameBase + RVA_GNET_CHAT_HANDLER));
    }
}
