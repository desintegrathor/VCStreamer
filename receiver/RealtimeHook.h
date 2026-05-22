#pragma once
#include <Windows.h>

// Initialize raw packet sniffing.
// No longer patches game code - packet sniffing is done from TickDelayBuffer.
bool InitRealtimeHook(uintptr_t baseGame);

void ShutdownRealtimeHook();

// Signal that all DLL initialization is complete and packet sniffing can begin.
void SetHookReady();

// Compatibility wrappers. New code should call ServerStateSniffer_OnRawMessage
// once per raw NET_ReadMessages packet before it enters the delay buffer.
void ScanBufferForKills(BYTE* data, DWORD size);
void ScanBufferForFlags(BYTE* data, DWORD size);
void ScanBufferForPlayerPositions(BYTE* data, DWORD size);

// Legacy name retained for render code. This returns a live-ahead authoritative
// position from ServerStateSniffer, not a predicted position.
bool RealtimeHook_GetPredictedPlayerPosition(int playerHandle, float outPos[3], DWORD maxAgeMs);
