#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

class GameChatSender {
public:
    static void Init(uintptr_t gameBase);
    static bool IsReady();
    static bool SendChatMessage(const std::string& text, bool teamChat);
};

