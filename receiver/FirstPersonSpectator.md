# First Person Spectator Mode - Implementační plán

## Shrnutí problému
Spectator režim v Vietcong multiplayeru používá third-person follow kameru. Cílem je implementovat skutečný first-person pohled, který zobrazuje přesně to, co vidí sledovaný hráč.

## Proč distance=0 nefunguje
Spectator kamera v Mode 2 počítá rotaci z **direction vectoru** (rozdíl mezi pozicí kamery a look-at bodem) pomocí `atan2`, NE z rotace hráče. I s nulovou vzdáleností by měla špatnou orientaci.

---

## Klíčové offsety a struktury

### Client struktura vs Player Entity
```c
// GNET_FindClientByEntityID vrací LPVOID (client structure)
// Player entity je na client+0xF4 (NE client[61]!)

LPVOID client = GNET_FindClientByEntityID(entityId);
if (client) {
    void* playerEntity = *(void**)((char*)client + 0xF4);  // +244 bytes
    if (playerEntity) {
        float* player = (float*)playerEntity;
        float pitch = player[61];  // playerEntity + 0xF4
        float yaw = player[63];    // playerEntity + 0xFC
    }
}
```

### Player Entity struktura
```c
struct PlayerEntity {
    int entity_id;              // +0x00
    // ...
    int stance_flag;            // +0x14 (20) - 0=crouching, non-zero=standing
    // ...
    float position_x;           // +0x34 (52)
    float position_y;           // +0x38 (56)
    float position_z;           // +0x3C (60)
    // ...
    float alt_pos_x;            // +0xD0 (208)
    float alt_pos_y;            // +0xD4 (212)
    float alt_pos_z;            // +0xD8 (216)
    // ...
    float pitch;                // +0xF4 (244) - VERTIKÁLNÍ rotace (radiány)
    // ...
    float yaw;                  // +0xFC (252) - HORIZONTÁLNÍ rotace (radiány)
    // ...
    void** skeleton_ptr;        // +0x378 (888) - pointer na skeleton array
    // ...
};
```

### Skeleton Bone System
```c
// Import: SKE_GetBoneEndLoc (0xF27E14)
// Signatura: int __cdecl SKE_GetBoneEndLoc(c_ske_skelet*, unsigned long boneId, c_Vector3* outPos)

// Bone IDs:
#define BONE_CHEST  0x0B  // 11 - Hrudník (používá GPLAYER_GetChestPosition)
#define BONE_HEAD   0x17  // 23 - Hlava (pro skutečný eye level)

// Jak získat skeleton:
void* playerEntity = ...;
void** skeletonArray = *(void**)((char*)playerEntity + 0x378);
void* skeleton = *skeletonArray;
```

---

## Klíčové funkce v game.dll

| Offset | Funkce | Popis |
|--------|--------|-------|
| 0xE87650 | `GNET_SpectatorCtrl_FillCamera` | Hlavní camera update pro spectator |
| 0xE8B590 | `GNET_FindClientByEntityID` | Najde client podle entity ID |
| 0xECBCB0 | `GPLAYER_GetChestPosition` | Pozice hrudníku (bone 11) |
| 0xECBA30 | `GPLAYER_GetYaw` | Vrací player[63] = yaw |
| 0xECBA40 | `GPLAYER_GetPitch` | Vrací player[61] = pitch |

### Import tabulka
| Offset | Funkce |
|--------|--------|
| 0xF27718 | `COM_I3DCamera_DirUp_From_RXYZ` - nastavení rotace kamery |
| 0xF27E14 | `SKE_GetBoneEndLoc` - pozice kosti |

### Globální proměnné
| Offset | Popis |
|--------|-------|
| 0x14EE9C8 | g_aActiveClients array |
| 0x14EE320 | SpectatorCtrl struktura base |
| 0x1500F58 | Game mode (10=spectator) |

---

## Mode 2 Assembly Flow (OVĚŘENO IDA ANALÝZOU)

### Vstupní bod Mode 2 - načtení rotace z kamery (NE z hráče!)
```asm
; Klíčové místo kde se načítá rotace - TOTO JE PROBLÉM!
0xE87793: mov eax, [edi+0F4h]     ; 8B 87 F4 00 00 00 - načte player entity z client+0xF4
0xE87799: test eax, eax           ; 85 C0 - kontrola null
0xE8779B: jz loc_E87DD5           ; 0F 84 34 06 00 00 - skok pokud null

; PROBLÉM: Rotace se načítá z ESI (camera struktura), NE z player entity!
0xE877A1: fld dword ptr [esi+0Ch] ; D9 46 0C - načítá YAW z camera struct!
0xE877A4: lea edx, [esp+var_D0]   ; 8D 54 24 20
0xE877A8: fld dword ptr [esi+8]   ; D9 46 08 - načítá PITCH z camera struct!
```

### Finální výpočet pitch/yaw (atan2 z direction vectoru)
```asm
; Výpočet PITCH z direction vectoru pomocí atan2
0xE87AF4: fmul [esp+var_CC]       ; direction.y * direction.y
0xE87AF8: fld [esp+var_D0]
0xE87AFC: fmul [esp+var_D0]       ; direction.x * direction.x
0xE87B00: faddp st(1), st         ; součet
0xE87B02: fsqrt                    ; sqrt(x² + y²)
0xE87B04: fpatan                   ; atan2(z, sqrt(x² + y²))
0xE87B06: fchs                     ; negace = pitch
0xE87B08: fstp [esp+var_D8]       ; D9 5C 24 18 - ULOŽENÍ PITCH

; Výpočet YAW z direction vectoru
0xE87B0C: fld [esp+var_D0]        ; D9 44 24 20 - načtení direction.x
0xE87B10: fchs                     ; D9 E0 - negace
0xE87B12: fld [esp+var_CC]        ; D9 44 24 24 - načtení direction.y
0xE87B16: fpatan                   ; D9 F3 - atan2(-x, y) = yaw
0xE87B18: fstp [esp+var_D4]       ; D9 5C 24 1C - ULOŽENÍ YAW
0xE87B1C: jmp loc_E87D95          ; E9 74 02 00 00 - skok na finální volání
```

### Hex dump klíčových oblastí
```
0xE87793: 8B 87 F4 00 00 00 85 C0 0F 84 34 06 00 00 D9 46
0xE877A3: 0C 8D 54 24 20 D9 46 08 ...

0xE87B08: D9 5C 24 18 D9 44 24 20 D9 E0 D9 44 24 24 D9 F3
0xE87B18: D9 5C 24 1C E9 74 02 00 00 ...
```

### Volání COM_I3DCamera_DirUp_From_RXYZ
```asm
0xE87D95: mov edx, [esp+1Ch]   ; 8B 54 24 1C - načte YAW z var_D4
0xE87D99: mov eax, [esp+18h]   ; 8B 44 24 18 - načte PITCH z var_D8
0xE87D9D: push edx             ; 52 - arg4: yaw
0xE87D9E: push 0               ; 6A 00 - arg3: roll (vždy 0)
0xE87DA0: push eax             ; 50 - arg2: pitch
0xE87DA1: push ebp             ; 55 - arg1: cameraProp
0xE87DA2: call [0xF27718]      ; FF 15 18 77 F2 00
```

### Registry v Mode 2 (OVĚŘENO)
| Registr/Offset | Obsah | Kdy validní |
|----------------|-------|-------------|
| EBX | SpectatorCtrl this | Celá funkce |
| EBP | cameraProp (I3D_CameraProp*) | Celá funkce |
| ESI | Camera entry pointer (20-byte struct) | Po 0xE876E3 |
| EDI | client pointer (z GNET_FindClientByEntityID) | Po 0xE87786 |
| EAX | player entity pointer | Po 0xE87793, před použitím |
| [ebx+52Ch] | Uložený client pointer | Po 0xE87AB6 |
| [esp+18h] | var_D8 = vypočítaný pitch | Po 0xE87B08 |
| [esp+1Ch] | var_D4 = vypočítaný yaw | Po 0xE87B18 |

### Problém a řešení
**PROBLÉM:** Na adrese 0xE877A1-0xE877A8 se načítá pitch/yaw z `ESI` (camera struktura),
ale player entity pointer je v `EAX` (načtený na 0xE87793).

**ŘEŠENÍ:** Přesměrovat načítání rotace z `[esi+8]`/`[esi+0Ch]` na `[eax+0F4h]`/`[eax+0FCh]`

---

## Implementace

### Varianta 1: Code Cave na 0xE87B1C (přepíše výsledky atan2)

**Patch na 0xE87B1C:**
- Původní: `E9 74 02 00 00` (jmp 0xE87D95)
- Nový: `E9 XX XX XX XX` (jmp code_cave)

```asm
code_cave:
    ; Zkontroluj jestli je first-person enabled
    cmp byte ptr [g_firstPersonEnabled], 0
    jz .original_path

    ; Načti client pointer (uložen v spectatorCtrl+0x52C)
    ; EBX = SpectatorCtrl this (validní v celé funkci)
    mov edi, [ebx+52Ch]
    test edi, edi
    jz .original_path

    ; Načti player entity z client+0xF4
    mov eax, [edi+0F4h]
    test eax, eax
    jz .original_path

    ; Načti player pitch do [esp+18h] (var_D8)
    mov edx, [eax+0F4h]       ; player pitch (offset 244)
    mov [esp+18h], edx

    ; Načti player yaw do [esp+1Ch] (var_D4)
    mov edx, [eax+0FCh]       ; player yaw (offset 252)
    mov [esp+1Ch], edx

.original_path:
    jmp 0xE87D95              ; Pokračuj na COM_I3DCamera_DirUp_From_RXYZ
```

### Varianta 2: Přímý patch na 0xE877A1 (změna zdroje rotace)

**Výhoda:** Nemusíme čekat na atan2 výpočet - rovnou načteme správné hodnoty.

**Problém:** `fld dword ptr [eax+0F4h]` potřebuje 6 bajtů, ale máme jen 3 bajty na instrukci.

**Řešení s code cave:**
```asm
; Patch na 0xE877A1 (3 bajty: D9 46 0C -> E9 XX XX + NOP padding)
; Přesměrovat na code cave který načte z player entity

original_0xE877A1:  ; Původní: fld [esi+0Ch]  - YAW z kamery
                    ; D9 46 0C (3 bajty)

original_0xE877A8:  ; Původní: fld [esi+8]   - PITCH z kamery
                    ; D9 46 08 (3 bajty)

; Code cave pro FP mode:
code_cave_rotation:
    cmp byte ptr [g_firstPersonEnabled], 0
    jz .use_camera_rotation

    ; EAX stále obsahuje player entity pointer (z 0xE87793)
    fld dword ptr [eax+0FCh]   ; player yaw
    fld dword ptr [eax+0F4h]   ; player pitch
    jmp return_address

.use_camera_rotation:
    fld dword ptr [esi+0Ch]    ; camera yaw (originál)
    fld dword ptr [esi+8]      ; camera pitch (originál)
    jmp return_address
```

### Varianta 3: Detour Hook (flexibilnější, doporučeno)

```cpp
// Definice typů
typedef int (__thiscall *FillCamera_t)(void* thisPtr, void* cameraProp);
FillCamera_t Original_FillCamera = nullptr;

// Globální proměnná pro toggle
bool g_firstPersonEnabled = false;

// Import funkce
typedef void (__cdecl *COM_I3DCamera_DirUp_From_RXYZ_t)(void* cameraProp, float pitch, float roll, float yaw);
COM_I3DCamera_DirUp_From_RXYZ_t COM_I3DCamera_DirUp_From_RXYZ = nullptr;

// Hook funkce
int __fastcall Hooked_FillCamera(void* thisPtr, void* edx_unused, void* cameraProp) {
    // Zavolej originál
    int result = Original_FillCamera(thisPtr, cameraProp);

    if (!result || !g_firstPersonEnabled) return result;

    // Zkontroluj Mode 2 (free camera following player)
    int mode = *(int*)thisPtr;
    if (mode != 2) return result;

    // Získej client pointer z spectatorCtrl+0x52C
    void* client = *(void**)((char*)thisPtr + 0x52C);
    if (!client) return result;

    // Získej player entity z client+0xF4
    void* playerEntity = *(void**)((char*)client + 0xF4);
    if (!playerEntity) return result;

    float* player = (float*)playerEntity;

    // Získej rotaci z player entity (radiány)
    float pitch = player[61];  // +0xF4 (offset 244)
    float yaw = player[63];    // +0xFC (offset 252)

    // Získej eye position pomocí GPLAYER_GetChestPosition
    float eyePos[3];
    typedef float* (__thiscall *GetChestPos_t)(void*, float*);
    auto GetChestPos = (GetChestPos_t)(g_gameBase + 0xECBCB0);
    GetChestPos(playerEntity, eyePos);

    // Přidej eye offset (výška očí nad hrudníkem)
    eyePos[2] += 0.3f;

    // Přepiš camera pozici
    float* camPos = (float*)cameraProp;
    camPos[0] = eyePos[0];  // +0x00: pos_x
    camPos[1] = eyePos[1];  // +0x04: pos_y
    camPos[2] = eyePos[2];  // +0x08: pos_z

    // Přepiš camera rotaci voláním COM_I3DCamera_DirUp_From_RXYZ
    // Signatura: void(I3D_CameraProp*, float pitch, float roll, float yaw)
    if (!COM_I3DCamera_DirUp_From_RXYZ) {
        // Načti z import tabulky
        COM_I3DCamera_DirUp_From_RXYZ = *(COM_I3DCamera_DirUp_From_RXYZ_t*)(g_gameBase + 0xF27718);
    }
    COM_I3DCamera_DirUp_From_RXYZ(cameraProp, pitch, 0.0f, yaw);

    return result;
}

// Instalace hooku (používá Microsoft Detours nebo MinHook)
void InstallFirstPersonHook(uintptr_t gameBase) {
    g_gameBase = gameBase;
    uintptr_t fillCameraAddr = gameBase + 0xE87650;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    Original_FillCamera = (FillCamera_t)fillCameraAddr;
    DetourAttach(&(PVOID&)Original_FillCamera, Hooked_FillCamera);
    DetourTransactionCommit();
}

void ToggleFirstPerson() {
    g_firstPersonEnabled = !g_firstPersonEnabled;
    // Log nebo notifikace
}
```

---

## Konstanty

```c
// Eye offset od chest position
#define EYE_OFFSET_FROM_CHEST 0.3f

// Camera distances (původní hodnoty)
#define CAMERA_DISTANCE_STANDING 7.0f
#define CAMERA_DISTANCE_CROUCHED 2.0f

// Spectator modes
#define SPECTATOR_MODE_TRACKING  0  // Statické kamery
#define SPECTATOR_MODE_SAVE      1  // Spline kamery
#define SPECTATOR_MODE_FREE      2  // Follow player (TOTO HOOKUJEME)
#define SPECTATOR_MODE_SPECIAL   3  // Network synchronized
```

---

## Kroky implementace

1. **Základní hook**
   - Implementovat detour na `GNET_SpectatorCtrl_FillCamera`
   - Přepsat rotaci kamery na player pitch/yaw
   - Přepsat pozici kamery na eye position

2. **Toggle mechanismus**
   - Globální proměnná `g_firstPersonEnabled`
   - Klávesová zkratka pro přepínání (např. F1)
   - Konzolový výstup aktuálního stavu

3. **Vylepšení**
   - Dynamický eye offset podle stance (standing/crouching/prone)
   - Smooth interpolace při přepínání režimů
   - Skrytí modelu sledovaného hráče v first-person

---

## Poznámky

- Rotace jsou v **radiánech**
- Player entity pitch je na offsetu +0xF4 (244 bytes) = `player[61]`
- Player entity yaw je na offsetu +0xFC (252 bytes) = `player[63]`
- `COM_I3DCamera_DirUp_From_RXYZ` má signaturu: `void(I3D_CameraProp*, float pitch, float roll, float yaw)`
- Roll je vždy 0
- Base adresa game.dll: typicky 0xD40000 (závisí na ASLR)
- Import tabulka je na relativním offsetu od base adresy

---

## Důležité offsety (souhrn)

| Struktura | Offset | Popis |
|-----------|--------|-------|
| Client | +0xF4 | Player entity pointer |
| PlayerEntity | +0x14 | Stance flag (0=crouching) |
| PlayerEntity | +0x34 | Position X |
| PlayerEntity | +0x38 | Position Y |
| PlayerEntity | +0x3C | Position Z |
| PlayerEntity | +0xF4 | Pitch (radiány) |
| PlayerEntity | +0xFC | Yaw (radiány) |
| PlayerEntity | +0x378 | Skeleton pointer |
| SpectatorCtrl | +0x00 | Mode (0,1,2,3) |
| SpectatorCtrl | +0x52C | Cached client pointer |
| I3D_CameraProp | +0x00 | Position X |
| I3D_CameraProp | +0x04 | Position Y |
| I3D_CameraProp | +0x08 | Position Z |
| I3D_CameraProp | +0x0C | FOV scale |

---

## Další kroky

1. Implementovat základní hook (Varianta 3)
2. Testovat s různými hráči v spectator módu
3. Přidat toggle klávesovou zkratku
4. Řešit edge cases (mrtvý hráč, změna sledovaného hráče)
5. Optimalizovat eye position podle stance
