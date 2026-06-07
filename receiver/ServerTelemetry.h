#pragma once

#include <Windows.h>
#include <vector>

constexpr unsigned int STREAMER_PLAYER_NAME_BYTES = 24;
constexpr unsigned int STREAMER_WEAPON_NAME_BYTES = 32;

constexpr unsigned int STREAMER_KILL_FLAG_HEADSHOT = 1u << 0;
constexpr unsigned int STREAMER_KILL_FLAG_TEAMKILL = 1u << 1;
constexpr unsigned int STREAMER_KILL_FLAG_SUICIDE = 1u << 2;
constexpr unsigned int STREAMER_FLAG_REASON_SCORE = 1u << 0;
constexpr unsigned int STREAMER_FLAG_REASON_KILLED = 1u << 1;
constexpr unsigned int STREAMER_FLAG_REASON_FROM_GROUND = 1u << 2;

struct ServerTelemetryKillEvent {
    unsigned long long sequence = 0;
    DWORD eventTick = 0;
    DWORD receivedTick = 0;
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
    DWORD receivedTick = 0;
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
    DWORD receivedTick = 0;
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

struct ServerTelemetryFlagEvent {
    unsigned long long sequence = 0;
    DWORD eventTick = 0;
    DWORD receivedTick = 0;
    int playerId = 0;
    int playerTeam = 0;
    unsigned int flags = 0;
    unsigned int flagSide = 255;
    unsigned int action = 0; // 1=take, 2=drop, 3=return
    unsigned int carryTimeMs = 0;
    float carryDistanceMeters = 0.0f;
    char playerName[STREAMER_PLAYER_NAME_BYTES] = {};
};

struct ServerTelemetryAchievementEvent {
    unsigned long long sequence = 0;
    DWORD eventTick = 0;
    DWORD receivedTick = 0;
    int playerId = 0;
    int playerTeam = 0;
    unsigned int flags = 0;
    unsigned int achievementId = 0;
    unsigned int triggerCount = 0;
    char playerName[STREAMER_PLAYER_NAME_BYTES] = {};
};

void InitServerTelemetry(uintptr_t gameBase);
void ServerTelemetry_Poll();
bool ServerTelemetry_TryFindKill(int killerId,
                                 int victimId,
                                 DWORD localTick,
                                 ServerTelemetryKillEvent* outEvent);
bool ServerTelemetry_TryFindFlag(int playerId,
                                 unsigned int action,
                                 DWORD localTick,
                                 ServerTelemetryFlagEvent* outEvent);
bool ServerTelemetry_TryFindAchievement(int playerId,
                                        DWORD localTick,
                                        ServerTelemetryAchievementEvent* outEvent);
void ServerTelemetry_CopyKills(std::vector<ServerTelemetryKillEvent>& out);
void ServerTelemetry_CopyHits(std::vector<ServerTelemetryHitEvent>& out);
void ServerTelemetry_CopyStats(std::vector<ServerTelemetryStatsDelta>& out);
void ServerTelemetry_CopyFlags(std::vector<ServerTelemetryFlagEvent>& out);
void ServerTelemetry_CopyAchievements(std::vector<ServerTelemetryAchievementEvent>& out);
