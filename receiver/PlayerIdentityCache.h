#pragma once

#include <Windows.h>
#include <cstddef>

struct PlayerIdentity {
    int handle = 0;
    int team = -1;
    char name[32] = {};
};

void PlayerIdentityCache_Init(uintptr_t gameBase);
void PlayerIdentityCache_Poll();
bool PlayerIdentityCache_Resolve(int handle, PlayerIdentity* out);
bool PlayerIdentityCache_IsResolvedName(const char* name);
