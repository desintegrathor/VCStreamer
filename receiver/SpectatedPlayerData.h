#pragma once
#include <Windows.h>

// Stance values (normalized)
constexpr int STANCE_STAND = 0;
constexpr int STANCE_CROUCH = 1;
constexpr int STANCE_PRONE = 2;

struct SpectatedPlayerState {
    int playerHandle = 0;

    // Weapon
    int weaponId = 0;
    const char* weaponName = "";
    int activeSlotIndex = -1;

    // Stance & animation
    int stance = STANCE_STAND;   // 0=stand, 1=crouch, 2=prone
    bool isFiring = false;
    int animId = 0;              // current 3PV animation ID
    int weaponState = 0;         // weapon state word from +706

    bool valid = false;
};

// Read combat-relevant state.
// clientStruct = playerTable[i] entry (stance, anim, fire at ntm offsets).
// playerEntity = clientStruct+0xF4 dereference (weapon slots).
SpectatedPlayerState ReadPlayerCombatState(void* clientStruct, void* playerEntity, uintptr_t gameBase);

// Get weapon name string from weapon ID (hardcoded table).
const char* GetWeaponName(int weaponId);
