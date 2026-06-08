#pragma once

#include "ServerStateSniffer.h"
#include <Windows.h>
#include <cstddef>
#include <vector>

struct PlayerInfo;

struct CommentaryCameraContext {
    DWORD currentTick = 0;
    int activeCameraTarget = 0;
    const char* shotKind = nullptr;

    int usCarrier = 0;
    int vcCarrier = 0;
    bool usCarrierMoving = false;
    bool vcCarrierMoving = false;
    bool usCarrierCamping = false;
    bool vcCarrierCamping = false;

    int flagScoreHoldTarget = 0;
    DWORD flagScoreHoldUntil = 0;

    int flagKillLookKillerHandle = 0;
    int flagKillLookVictimHandle = 0;
    DWORD flagKillLookStartTick = 0;
    DWORD flagKillLookKillTick = 0;

    bool targetAlive = true;
    bool targetRecentlyDied = false;
    int targetDeathKillerHandle = 0;
    DWORD targetDeathTick = 0;
    DWORD targetSinceTick = 0;
};

void CommentaryEngine_Init(uintptr_t gameBase);
void CommentaryEngine_Reset();
void CommentaryEngine_UpdateScoreboard(const std::vector<PlayerInfo>& players);
void CommentaryEngine_OnCommittedDirectorEvent(const DirectorEvent& event,
                                               DWORD now,
                                               int activeCameraTarget,
                                               const char* shotKind);
void CommentaryEngine_OnFocusDeath(int victimHandle,
                                   int killerHandle,
                                   DWORD deathTick,
                                   DWORD now);
void CommentaryEngine_Update(const CommentaryCameraContext& context);
bool CommentaryEngine_GetActiveLine(char* outText, size_t outTextLen, float* outAlpha);
