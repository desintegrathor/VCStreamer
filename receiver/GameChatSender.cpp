#include "GameChatSender.h"
#include "DiagnosticsLog.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr uintptr_t RVA_GNET_SEND_MESSAGE_TO_PLAYER = 0x14B640;
constexpr uintptr_t RVA_G_DP_SERVER_HANDLE = 0x20DA80;
constexpr uintptr_t RVA_G_DP_CONNECTION = 0x20DA84;
constexpr uintptr_t RVA_MP_SUBSTATE = 0x7C0F58;
constexpr unsigned char CHAT_PACKET_TYPE = 35;
constexpr DWORD FALLBACK_SERVER_HANDLE = 1;
constexpr size_t MAX_CHAT_CHARS = 80;

uintptr_t g_gameBase = 0;

using GnetSendMessageToPlayerFn = void(__cdecl*)(void* targetHandle, char* data, unsigned int size, int reliable, float delaySeconds);

bool SafeReadDword(uintptr_t address, DWORD& value) {
    __try {
        value = *(DWORD*)address;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
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

void WriteBits(std::vector<unsigned char>& buffer, int& bitPos, unsigned int value, int bitCount) {
    for (int i = 0; i < bitCount; ++i) {
        if ((value >> i) & 1u) {
            buffer[(size_t)bitPos >> 3] |= (unsigned char)(1u << (bitPos & 7));
        }
        ++bitPos;
    }
}

std::vector<unsigned char> BuildClientChatPacket(const std::string& text, bool teamChat) {
    std::vector<unsigned char> packet(96, 0);
    int bitPos = 0;

    WriteBits(packet, bitPos, CHAT_PACKET_TYPE, 8);
    WriteBits(packet, bitPos, teamChat ? 1u : 0u, 1);

    const size_t count = (std::min)(text.size(), MAX_CHAT_CHARS);
    for (size_t i = 0; i < count; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32 || ch > 126) {
            ch = ' ';
        }
        WriteBits(packet, bitPos, ch & 0x7Fu, 7);
    }
    WriteBits(packet, bitPos, 0, 7);

    packet.resize(((size_t)bitPos + 7) / 8);
    return packet;
}

bool CallGameSend(uintptr_t gameBase, DWORD targetHandle, unsigned char* data, unsigned int size) {
    __try {
        auto sendFn = (GnetSendMessageToPlayerFn)(gameBase + RVA_GNET_SEND_MESSAGE_TO_PLAYER);
        sendFn((void*)targetHandle, (char*)data, size, 1, 0.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

void GameChatSender::Init(uintptr_t gameBase) {
    g_gameBase = gameBase;
}

bool GameChatSender::IsReady() {
    if (!g_gameBase) {
        return false;
    }

    DWORD connection = 0;
    int mpSubState = 0;

    if (!SafeReadDword(g_gameBase + RVA_G_DP_CONNECTION, connection) || connection == 0) {
        return false;
    }
    if (!SafeReadInt(g_gameBase + RVA_MP_SUBSTATE, mpSubState) || mpSubState != 10) {
        return false;
    }

    return true;
}

bool GameChatSender::SendChatMessage(const std::string& text, bool teamChat) {
    if (text.empty() || !IsReady()) {
        return false;
    }

    DWORD serverHandle = 0;
    if (!SafeReadDword(g_gameBase + RVA_G_DP_SERVER_HANDLE, serverHandle) || serverHandle == 0) {
        // Spectator clients commonly have this global cleared even though
        // DirectPlay accepts handle 1 as the server target.
        serverHandle = FALLBACK_SERVER_HANDLE;
    }

    std::vector<unsigned char> packet = BuildClientChatPacket(text, teamChat);
    if (packet.empty()) {
        return false;
    }

    if (!CallGameSend(g_gameBase, serverHandle, packet.data(), (unsigned int)packet.size())) {
        DiagnosticsLog_Append("receiver_debug.log", "[YouTubeChat] GNET_SendMessageToPlayer failed\n");
        return false;
    }

    return true;
}
