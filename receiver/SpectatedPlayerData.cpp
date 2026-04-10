#include "SpectatedPlayerData.h"

// ============================================================================
// All offsets are on the PLAYER ENTITY (client + 0xF4 dereference).
// The ntm_messages.md offsets (+700 etc.) are SERVER-SIDE only and do not
// exist on the client entity.
// ============================================================================

// Stance (from animation-system-research.md §2.3, §4.1)
constexpr int ENT_STANCE_FLAG  = 0x14;  // DWORD — 0=crouch, nonzero=stand (per research doc)

// Animation layers (from GAM_PL_UpdateAnimLayers decompile)
constexpr int ENT_ANIM_LAYER_COUNT = 0x500;  // DWORD — number of active anim layers
constexpr int ENT_ANIM_LAYER_BASE  = 0x50C;  // first layer's anim_id (DWORD), 32 bytes per layer

// Weapon slots (from vcguardupgrade weapon_system.md — confirmed working)
constexpr int ENT_WEAPON_IDS   = 0x3A8;  // DWORD[20] — weapon IDs per slot
constexpr int ENT_WEAPON_PTRS  = 0x3F8;  // DWORD[20] — weapon object pointers per slot
constexpr int ENT_ACTIVE_WEAP  = 0x448;  // DWORD*    — currently active weapon slot pointer
constexpr int WEAPON_SLOT_COUNT = 20;

// ============================================================================
// Stance from anim ID (from GAM_PL_GetMoveAnimID ranges)
// ============================================================================

static int StanceFromAnimId(int animId) {
    if (animId >= 400 && animId < 600) return STANCE_PRONE;
    if (animId >= 300 && animId < 400) return STANCE_CROUCH;
    // 0-199, 200-299, 600+ are all standing or action overlays
    return STANCE_STAND;
}

// ============================================================================
// Weapon name lookup
// ============================================================================

const char* GetWeaponName(int weaponId) {
    switch (weaponId) {
        case 1:  return "M16";
        case 2:  return "AK-47";
        case 3:  return "Skorpion";
        case 4:  return "M1 Garand";
        case 5:  return "DP-28";
        case 6:  return "PPS-41";
        case 7:  return "M1911";
        case 8:  return "Tokarev";
        case 9:  return "Makarov";
        case 10: return "Revolver";
        case 11: return "Remington";
        case 12: return "Winchester";
        case 13: return "SVT-40";
        case 14: return "Dragunov";
        case 15: return "Simonov";
        case 16: return "SVT-40 Optics";
        case 17: return "M60";
        case 18: return "Degtyarev";
        case 19: return "M3 Grease Gun";
        case 20: return "C4";
        case 21: return "Thompson";
        case 22: return "SW39";
        case 23: return "PPS-43";
        case 24: return "M14";
        case 25: return "M1 Carbine";
        case 26: return "Mosin-Nagant";
        case 27: return "M79";
        case 28: return "Baikal";
        case 29: return "Knife (US)";
        case 30: return "Knife (VC)";
        case 31: return "M14 Optics";
        case 32: return "Sten";
        case 33: return "Machete";
        case 50: return "Grenade (VC)";
        case 59: return "Grenade (US)";
        default: return "Unknown";
    }
}

// ============================================================================
// Read player combat state — all from entity
// ============================================================================

SpectatedPlayerState ReadPlayerCombatState(void* clientStruct, void* playerEntity, uintptr_t gameBase) {
    SpectatedPlayerState state;
    (void)clientStruct; // unused now — kept for API compat
    if (!playerEntity) return state;

    BYTE* ent = (BYTE*)playerEntity;

    __try {
        // --- Animation layer: read base anim ID ---
        DWORD layerCount = *(DWORD*)(ent + ENT_ANIM_LAYER_COUNT);
        if (layerCount > 0 && layerCount < 32) {
            state.animId = *(DWORD*)(ent + ENT_ANIM_LAYER_BASE);
        }

        // --- Stance: derive from anim ID ---
        state.stance = StanceFromAnimId(state.animId);

        // --- Stance flag at +0x14 (secondary, for logging) ---
        state.weaponState = *(DWORD*)(ent + ENT_STANCE_FLAG);

        // --- Active weapon: find which slot matches ---
        DWORD activeWeapPtr = *(DWORD*)(ent + ENT_ACTIVE_WEAP);
        state.activeSlotIndex = -1;
        state.weaponId = 0;

        if (activeWeapPtr != 0) {
            DWORD* slotPtrs = (DWORD*)(ent + ENT_WEAPON_PTRS);
            DWORD* slotIds  = (DWORD*)(ent + ENT_WEAPON_IDS);

            for (int i = 0; i < WEAPON_SLOT_COUNT; i++) {
                if (slotPtrs[i] == activeWeapPtr) {
                    state.activeSlotIndex = i;
                    state.weaponId = (int)slotIds[i];
                    break;
                }
            }
        }

        state.weaponName = GetWeaponName(state.weaponId);
        state.valid = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        state.valid = false;
    }

    return state;
}
