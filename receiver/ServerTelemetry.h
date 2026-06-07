#pragma once

#include <Windows.h>
#include <vector>

constexpr unsigned int STREAMER_PLAYER_NAME_BYTES = 24;
constexpr unsigned int STREAMER_WEAPON_NAME_BYTES = 32;

struct ServerTelemetryKillEvent {
    unsigned long long sequence = 0;
    DWORD eventTick = 0;
    int killerId = 0;
    int victimId = 0;
    int killerTeam = 0;
    int victimTeam = 0;
    int weaponId = 0;
    unsigned int flags = 0;
    char killerName[STREAMER_PLAYER_NAME_BYTES] = {};
    char victimName[STREAMER_PLAYER_NAME_BYTES] = {};
    char weaponName[STREAMER_WEAPON_NAME_BYTES] = {};
};

struct ServerTelemetryHitEvent {
    unsigned long long sequence = 0;
    DWORD eventTick = 0;
    int attackerId = 0;
    int victimId = 0;
    int attackerTeam = 0;
    int victimTeam = 0;
    int weaponId = 0;
    unsigned int flags = 0;
    float damageHP = 0.0f;
    float distanceMeters = 0.0f;
    unsigned int boneMask = 0;
    char attackerName[STREAMER_PLAYER_NAME_BYTES] = {};
    char victimName[STREAMER_PLAYER_NAME_BYTES] = {};
    char weaponName[STREAMER_WEAPON_NAME_BYTES] = {};
};

struct ServerTelemetryStatsDelta {
    unsigned long long sequence = 0;
    DWORD eventTick = 0;
    int playerId = 0;
    int team = 0;
    char playerName[STREAMER_PLAYER_NAME_BYTES] = {};
    unsigned int teamkills = 0;
    unsigned int flagAttempts = 0;
    unsigned int flagTimeSec = 0;
    unsigned int aliveTimeSec = 0;
    unsigned int distanceMeters = 0;
    unsigned int flagDistanceMeters = 0;
    unsigned int accuracyShots = 0;
    unsigned int accuracyHits = 0;
    unsigned int accuracyHeadshots = 0;
    unsigned int totalDamage = 0;
};

void InitServerTelemetry(uintptr_t gameBase);
void ServerTelemetry_Poll();
bool ServerTelemetry_TryFindKill(int killerId,
                                 int victimId,
                                 DWORD localTick,
                                 ServerTelemetryKillEvent* outEvent);
void ServerTelemetry_CopyKills(std::vector<ServerTelemetryKillEvent>& out);
void ServerTelemetry_CopyHits(std::vector<ServerTelemetryHitEvent>& out);
void ServerTelemetry_CopyStats(std::vector<ServerTelemetryStatsDelta>& out);
