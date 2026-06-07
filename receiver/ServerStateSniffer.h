#pragma once
#include <Windows.h>
#include <vector>

enum class DirectorEventType {
    Kill,
    Flag
};

struct PlayerSnapshot {
    int handle = 0;
    DWORD rawTick = 0;
    DWORD visibleTick = 0;
    float pos[3] = {};
    bool hasPosition = false;
};

struct DirectorEvent {
    DirectorEventType type = DirectorEventType::Kill;
    unsigned long long sequence = 0;

    DWORD rawTick = 0;
    DWORD visibleTick = 0;
    int playbackDelayMs = 0;
    int availableLeadMs = 0;

    int killerHandle = 0;
    int victimHandle = 0;
    int weaponId = 0;
    bool hasServerTelemetry = false;
    char serverWeaponName[32] = {};
    unsigned int serverTelemetryFlags = 0;

    int usCarrier = 0;
    int vcCarrier = 0;
    int previousUSCarrier = 0;
    int previousVCCarrier = 0;
    bool hasFlagTelemetry = false;
    unsigned int flagAction = 0; // 1=take, 2=drop, 3=return
    unsigned int flagSide = 255;
    unsigned int flagReasonFlags = 0;
    unsigned int flagCarryTimeMs = 0;
    float flagCarryDistanceMeters = 0.0f;
    int flagPlayerHandle = 0;
    int flagPlayerTeam = -1;
    char flagPlayerName[24] = {};

    PlayerSnapshot killerSnapshot;
    PlayerSnapshot victimSnapshot;
    PlayerSnapshot usCarrierSnapshot;
    PlayerSnapshot vcCarrierSnapshot;
    bool hasKillerSnapshot = false;
    bool hasVictimSnapshot = false;
    bool hasUSCarrierSnapshot = false;
    bool hasVCCarrierSnapshot = false;
};

void InitServerStateSniffer(uintptr_t gameBase);
void ShutdownServerStateSniffer();

// Called from Hooked_NET_ReadMessages with raw server data before local buffering.
void ServerStateSniffer_OnRawMessage(BYTE* data, DWORD size);

// Moves pending normalized kill/flag events into out.
void ServerStateSniffer_DrainEvents(std::vector<DirectorEvent>& out);

// Returns the nearest live-ahead snapshot for handle at a future visible tick.
bool ServerStateSniffer_GetSnapshotAt(int handle, DWORD visibleTick, PlayerSnapshot* out);
