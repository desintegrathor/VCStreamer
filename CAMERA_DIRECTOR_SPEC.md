# VCStreamer Camera Director — Centralized Redesign Specification

## Executive Summary

The current camera switching logic is scattered across **6+ files** with overlapping responsibilities, race conditions, and no unified decision-making. This spec proposes a new **CameraDirector** — a single centralized module that owns all camera switching decisions, replacing the fragmented logic currently distributed across `SpectatorController`, `FirstPersonCamera`, `WorldCameraTracker`, `FlagMonitor`, and `RealtimeHook`.

The key improvement: **distance-based cinematic kill probability** — kills at 25m+ get a 30% cinematic chance, scaling linearly to 70% at 50m+, making long-range kills feel dramatic while keeping close-range combat flowing naturally.

---

## 1. Current Architecture — Problems

### 1.1 Where Camera Decisions Are Made Today

| File | What It Decides | Problem |
|------|----------------|---------|
| `SpectatorController.cpp` | Kill → switch to killer, random KillCam roll (`rand() % 100 < fpvChance`) | Flat probability, no distance awareness |
| `FirstPersonCamera.cpp` | KillCam phase execution (9s sequence), 3PV smoothing, player change detection | Mixes decision-making with execution |
| `WorldCameraTracker.cpp` | World camera scoring/selection (static/dynamic), player cam fallback | Independent system, not coordinated with kills |
| `FlagMonitor.cpp` | Flag carrier polling from game memory | Duplicates network-based flag detection |
| `RealtimeHook.cpp` | Kill/flag detection from network packets, delay scheduling | Good — detection layer, keep as-is |
| `DelayManager.cpp` | Delay queue, immediate execution for short delays | Good — timing layer, keep as-is |

### 1.2 Specific Issues

**Scattered decision-making:** `ProcessKillEvent()` in SpectatorController decides *whether* to KillCam, but `Hooked_FillCamera()` in FirstPersonCamera decides *how*. `WorldCameraTracker_Update()` independently switches between world/player cameras with no awareness of kill events or KillCam state.

**No distance awareness:** The `fpvChance` is a flat percentage (default 50%). A point-blank pistol kill and a 100m sniper kill get the same cinematic treatment. Long-range kills deserve more dramatic presentation.

**Race conditions:** `WorldCameraTracker` uses `atomic<int>` for `g_targetHandle` but surrounding state (`g_currentCamIndex`, `g_holdFrames`, `g_switchCooldownFrames`) is unprotected. `FlagMonitor` polls game memory on its own thread while `RealtimeHook` detects flag changes from network packets — potential desync.

**Dual flag systems:** Both `FlagMonitor.cpp` (memory polling every 200ms) and `RealtimeHook.cpp` (network packet scanning for `MSG_STATE_VAR`) track flag carriers. These can fire conflicting events.

**No priority system:** Kill events and flag events compete with no formal priority. A cinematic KillCam can be interrupted by a flag pickup. World camera switches happen independently during KillCam sequences.

**Hardcoded magic numbers everywhere:** `5` second cooldown, `9` second KillCam, `18m` world cam distance, `240` frame switch cooldown, `600` frame max hold — all hardcoded with no central configuration.

---

## 2. New Architecture — The CameraDirector

### 2.1 Design Philosophy

Inspired by professional broadcast systems and game cinematography research:

- **Single point of truth:** One module decides what the camera does. Period.
- **Priority-driven event queue:** Events (kills, flags, world cam suggestions) are scored and the highest-priority event wins.
- **Distance-aware cinematic probability:** Long-range kills get dramatic treatment; close-range stays fast.
- **Decay-based priorities:** Event importance fades over time — a 5-second-old kill is less interesting than a fresh flag grab.
- **State machine execution:** Clean transitions between camera states with no phase-juggling inside hook functions.
- **The "invisible camera" principle:** The best camera work is the kind viewers don't consciously notice — smooth transitions, no jarring cuts, always showing the right thing at the right time.

### 2.2 Module Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                        CameraDirector.cpp/.h                        │
│                    (NEW — single centralized file)                   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────────┐   ┌──────────────────┐   ┌────────────────┐  │
│  │  Event Receiver   │   │  Priority Queue  │   │  State Machine │  │
│  │                   │   │                  │   │                │  │
│  │  OnKillDetected() │──▶│  Scored events   │──▶│  Idle          │  │
│  │  OnFlagChanged()  │   │  with decay      │   │  FollowPlayer  │  │
│  │  OnWorldCamTick() │   │  time-windowed   │   │  KillCam       │  │
│  │  OnPlayerDied()   │   │                  │   │  WorldCam      │  │
│  └──────────────────┘   └──────────────────┘   │  FlagWatch     │  │
│                                                 └────────────────┘  │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  CameraConfig (loaded from vcstreamer.ini)                   │   │
│  │  All tuning parameters in one place                          │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  Distance Calculator                                         │   │
│  │  GetKillDistance(killerHandle, victimHandle) → float meters   │   │
│  │  ComputeCinematicChance(distance) → float 0.0–1.0            │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘

External systems that FEED events into CameraDirector (unchanged):
  RealtimeHook.cpp → kill/flag detection from network
  DelayManager.cpp → delay queue and timing
  GameMemoryReader.cpp → player positions, entity data

External systems that EXECUTE camera commands from CameraDirector (simplified):
  FirstPersonCamera.cpp → Hooked_FillCamera() executes whatever state CameraDirector says
  WorldCameraTracker.cpp → becomes a passive camera scorer, no longer switches on its own
  SpectatorController.cpp → becomes a thin wrapper for SetSpectatorToPlayerId() only
```

### 2.3 Files to Create

| File | Purpose |
|------|---------|
| `CameraDirector.cpp` | All decision logic, state machine, event processing, distance calculation |
| `CameraDirector.h` | Public API, enums, config struct |

### 2.4 Files to Modify

| File | Changes |
|------|---------|
| `SpectatorController.cpp` | **Remove** `ProcessKillEvent()` decision logic (keep `SetSpectatorToPlayerId()` as a utility). Remove `ProcessFlagEvent()` flag watcher thread. Remove cooldown logic. These all move to CameraDirector. |
| `SpectatorController.h` | Remove `ProcessKillEvent()`, `ProcessFlagEvent()` from public API. Keep `SetSpectatorToPlayerId()`, `UpdateScoreboard()`, `PatchCameraDistance()`. |
| `FirstPersonCamera.cpp` | **Remove** KillCam decision/state management. `Hooked_FillCamera()` reads state from `CameraDirector` instead of managing its own `g_currentCameraMode`, `g_killCamPhase`, etc. Keeps the actual camera math (3PV smoothing, KillCam slide interpolation) as execution code. |
| `FirstPersonCamera.h` | Remove `StartKillCam()`. Add `GetCameraDirectorState()` or similar accessor. |
| `WorldCameraTracker.cpp` | **Remove** autonomous switching logic. Convert `WorldCameraTracker_Update()` to be a passive scorer: `WorldCameraTracker_ScoreBestCamera()` returns the best world camera and its score. CameraDirector calls this and decides whether to use it. |
| `WorldCameraTracker.h` | Replace `SetTarget/ClearTarget/Update` with `ScoreBestCamera()` API. |
| `FlagMonitor.cpp` | **Remove entirely** — flag detection from `RealtimeHook.cpp` (network packets) is the single source of truth. No more memory polling. |
| `DelayManager.cpp` | Route `ProcessActions()` kill/flag callbacks to `CameraDirector` instead of `SpectatorController`. |
| `DelayManager.h` | Add new INI config keys for the CameraDirector (see section 4). |
| `dllmain.cpp` | Add `InitCameraDirector(base)` to startup sequence. Remove `InitFlagMonitor(base)`. |

### 2.5 Files to Delete

| File | Reason |
|------|--------|
| `FlagMonitor.cpp` | Replaced by RealtimeHook network-based flag detection, routed through CameraDirector |
| `FlagMonitor.h` | Same |

---

## 3. Distance-Based Cinematic Kill System

### 3.1 Core Formula

```cpp
float ComputeCinematicChance(float killDistanceMeters) {
    // Below 25m: no cinematic (too close, keep action flowing)
    if (killDistanceMeters < 25.0f) return 0.0f;

    // 25m–50m: linear ramp from 30% to 70%
    if (killDistanceMeters < 50.0f) {
        float t = (killDistanceMeters - 25.0f) / 25.0f;  // 0.0 at 25m, 1.0 at 50m
        return 0.30f + t * 0.40f;  // 0.30 at 25m, 0.70 at 50m
    }

    // 50m+: full cinematic chance (70%)
    return 0.70f;
}
```

**Visual breakdown:**

```
Cinematic Chance
  70% |                          ___________________
      |                        /
      |                      /
  30% |____________________/
      |
   0% |____________________|____|____|____|____|____
      0m       10m      25m   30m   40m   50m   60m
                          ↑                 ↑
                     Threshold         Full chance
                   (30% chance)       (70% chance)
```

### 3.2 Distance Measurement

```cpp
float GetKillDistance(int killerHandle, int victimHandle) {
    void* killerEntity = FindEntityByHandle(killerHandle);
    void* victimEntity = FindEntityByHandle(victimHandle);

    if (!killerEntity || !victimEntity) return 0.0f;

    float killerPos[3], victimPos[3];
    GetEntityPos(killerEntity, killerPos);
    GetEntityPos(victimEntity, victimPos);

    float dx = killerPos[0] - victimPos[0];
    float dy = killerPos[1] - victimPos[1];
    float dz = killerPos[2] - victimPos[2];

    return sqrtf(dx*dx + dy*dy + dz*dz);
}
```

**Important timing note:** By the time the delayed kill event fires (accounting for spectator delay), both entities should still be alive at approximately the right positions. The distance is measured at event processing time, which corresponds roughly to the moment visible on-screen.

### 3.3 Why This Works Cinematically

Based on research into professional game camera systems and esports broadcasting:

- **Close range (< 25m):** Fast-paced combat. Cinematic cameras would disrupt the flow and feel jarring. The "invisible camera" principle says: don't interrupt action with forced camera changes. Keep 3PV following the killer smoothly.

- **Medium range (25–50m):** The kill had intent — a deliberate shot across a clearing, a well-placed burst through jungle. The distance creates natural dramatic tension. A 30–70% scaling chance means not every medium kill gets cinematic treatment, keeping it feeling special.

- **Long range (50m+):** Sniper kills, long-range headshots. These are the highlight-reel moments. At 70% chance (not 100%), there's still variety — sometimes the camera just follows the killer, which also looks cool with the player scanning the horizon after a long shot.

- **Never 100%:** Even at max distance, 30% of the time we stay in normal mode. This prevents the cinematic from becoming predictable and keeps the broadcast feeling organic, as a real camera operator would vary their shots.

---

## 4. Camera State Machine

### 4.1 States

```cpp
enum class CameraState {
    Idle,           // No target, waiting for events
    FollowPlayer,   // 3PV on a player (default after kill/flag)
    KillCam,        // Cinematic kill sequence (phased: wait → slide → attached)
    WorldCam,       // Static/dynamic world camera tracking player
    FlagWatch       // Following flag carrier (higher priority than kills)
};
```

### 4.2 State Transitions

```
                    ┌──────────────────────────────┐
                    │            Idle               │
                    │  (waiting for first event)    │
                    └──────────┬───────────────────┘
                               │ any event
                               ▼
    ┌───────────── FollowPlayer ◄────────────────────────┐
    │              (default state)                        │
    │                │        │        │                  │
    │      kill      │   flag │  world │            timeout/
    │    (distance   │  grab  │  cam   │            end   │
    │     roll wins) │        │  good  │                  │
    │                ▼        ▼        ▼                  │
    │           KillCam   FlagWatch  WorldCam ────────────┤
    │              │          │        │                   │
    │         9s timeout  flag lost  hold expired          │
    │              │     (3s grace)    │                   │
    │              ▼          ▼        ▼                   │
    │         FollowPlayer  FollowPlayer  FollowPlayer    │
    │                                                     │
    │     ┌─── HIGH PRIORITY INTERRUPT ────┐              │
    │     │ Flag events can interrupt any   │              │
    │     │ state except active KillCam     │──────────────┘
    │     │ phase 1 (slide transition)      │
    │     └────────────────────────────────┘
    │
    └─────────────────────────────────────────────────────┘
```

### 4.3 Priority System

Events have base priorities that decay over time:

```cpp
struct CameraEvent {
    enum class Type { Kill, FlagGrab, FlagLost, WorldCamSuggestion };

    Type type;
    float basePriority;
    float decayRate;        // priority loss per second
    DWORD timestamp;

    // Kill-specific
    int killerHandle;
    int victimHandle;
    float killDistance;
    bool cinematicEligible; // passed the distance roll

    // Flag-specific
    int flagCarrierHandle;

    float GetPriority(DWORD now) const {
        float elapsed = (now - timestamp) / 1000.0f;
        return basePriority - (decayRate * elapsed);
    }
};
```

**Base priorities:**

| Event Type | Base Priority | Decay Rate | Rationale |
|-----------|--------------|------------|-----------|
| Kill (cinematic eligible) | 100 | 10/s | Highest — long-range kill gets full treatment |
| Kill (normal) | 60 | 15/s | Standard kill, switch to killer |
| Flag Grab | 80 | 5/s | Very important, slow decay — keep watching carrier |
| Flag Lost | 40 | 20/s | Brief interest, fast decay |
| World Cam Suggestion | 30 | 3/s | Ambient variety, lowest priority |

**Decision logic in CameraDirector::Update():**

```cpp
void CameraDirector::Update() {
    DWORD now = GetTickCount();

    // 1. If in KillCam and not interruptible, let it run
    if (m_state == CameraState::KillCam && !IsKillCamInterruptible()) {
        return;
    }

    // 2. Find highest-priority pending event
    CameraEvent* best = nullptr;
    float bestPriority = 0.0f;

    for (auto& event : m_pendingEvents) {
        float p = event.GetPriority(now);
        if (p > bestPriority && p > 0.0f) {
            best = &event;
            bestPriority = p;
        }
    }

    // 3. Clean expired events (priority <= 0)
    PruneExpiredEvents(now);

    // 4. If no event beats current state's hold priority, stay
    if (!best || bestPriority < GetCurrentStateHoldPriority()) {
        return;
    }

    // 5. Execute transition
    TransitionToEvent(*best);
}
```

### 4.4 KillCam Interruptibility

The KillCam should only be interrupted during safe windows:

- **Phase 0 (wait, 0–3.5s):** Interruptible by flag events (priority 80+)
- **Phase 1 (slide, 3.5–5.0s):** **NOT interruptible** — mid-transition cuts look terrible
- **Phase 2 (attached, 5.0–9.0s):** Interruptible by any high-priority event after 6.0s

```cpp
bool CameraDirector::IsKillCamInterruptible() const {
    float elapsed = (GetTickCount() - m_killCamStartTick) / 1000.0f;

    if (elapsed < KILLCAM_WAIT_END) return true;           // Phase 0: interruptible
    if (elapsed < KILLCAM_TRANSITION_END) return false;     // Phase 1: NO
    if (elapsed > 6.0f) return true;                        // Phase 2 after 1s: yes
    return false;                                            // Phase 2 first second: no
}
```

---

## 5. Detailed Implementation Plan

### 5.1 CameraDirector.h

```cpp
#pragma once
#include <Windows.h>
#include <vector>
#include <mutex>
#include <atomic>

// ============================================================================
// Configuration — all tunables loaded from vcstreamer.ini
// ============================================================================

struct CameraConfig {
    // Distance-based cinematic thresholds
    float cinematicMinDistance      = 25.0f;   // Below this: 0% cinematic chance
    float cinematicMaxDistance      = 50.0f;   // Above this: max cinematic chance
    float cinematicMinChance       = 0.30f;   // Chance at min distance (30%)
    float cinematicMaxChance       = 0.70f;   // Chance at max distance (70%)

    // KillCam timing (seconds)
    float killCamWaitDuration      = 3.5f;    // Phase 0: 3PV on killer
    float killCamTransitionDuration = 1.5f;   // Phase 1: slide to victim
    float killCamAttachedDuration  = 4.0f;    // Phase 2: 3PV on victim
    float killCamSlideHeight       = 0.8f;    // Camera height during slide

    // Cooldowns (seconds)
    float killCooldown             = 5.0f;    // Block new kills after switching
    float flagLostGracePeriod      = 3.0f;    // Keep watching carrier after flag lost

    // World camera
    float worldCamMaxDistance       = 18.0f;   // Max distance for world cam selection
    float worldCamSwitchCooldown   = 4.0f;    // Seconds between world cam switches
    float worldCamMaxHold          = 10.0f;   // Seconds before forced variety switch
    float worldCamLOSPenalty       = 60.0f;   // Score penalty for blocked line of sight
    float worldCamStickiness       = 5.0f;    // Score bonus for current camera

    // 3PV smoothing
    float tpvMaxDistance           = 2.5f;    // Max 3PV camera distance
    float tpvYawSmoothFactor      = 0.01f;   // Yaw interpolation speed
    float tpvZoomInFactor          = 0.3f;    // Distance zoom-in speed
    float tpvZoomOutFactor         = 0.005f;  // Distance zoom-out speed

    // Priority weights
    float priorityKillCinematic    = 100.0f;
    float priorityKillNormal       = 60.0f;
    float priorityFlagGrab         = 80.0f;
    float priorityFlagLost         = 40.0f;
    float priorityWorldCam         = 30.0f;

    // Debug
    bool debugMode                 = false;
};

// ============================================================================
// Camera State
// ============================================================================

enum class CameraState {
    Idle,
    FollowPlayer,
    KillCam,
    WorldCam,
    FlagWatch
};

// KillCam sub-phases
enum class KillCamPhase {
    Wait,        // 3PV on killer
    Transition,  // Slide from killer to victim
    Attached     // 3PV on victim
};

// ============================================================================
// Camera Event
// ============================================================================

struct CameraEvent {
    enum class Type { Kill, FlagGrab, FlagLost };

    Type type;
    float basePriority;
    float decayRate;
    DWORD timestamp;

    // Kill data
    int killerHandle = 0;
    int victimHandle = 0;
    float killDistance = 0.0f;
    bool cinematicEligible = false;

    // Flag data
    int usCarrier = 0;
    int vcCarrier = 0;

    float GetPriority(DWORD now) const {
        float elapsed = (now - timestamp) / 1000.0f;
        float p = basePriority - (decayRate * elapsed);
        return (p > 0.0f) ? p : 0.0f;
    }
};

// ============================================================================
// Public API
// ============================================================================

// Lifecycle
void InitCameraDirector(uintptr_t gameBase);
void ShutdownCameraDirector();

// Event input (called by DelayManager when delayed actions fire)
void CameraDirector_OnKill(int killerHandle, int victimHandle);
void CameraDirector_OnFlagChanged(int usCarrier, int vcCarrier);

// Per-frame update (called from main loop, ~100ms interval)
void CameraDirector_Update();

// State query (called by Hooked_FillCamera to know what to render)
CameraState CameraDirector_GetState();
KillCamPhase CameraDirector_GetKillCamPhase();
int CameraDirector_GetTargetHandle();
int CameraDirector_GetKillCamKillerHandle();
int CameraDirector_GetKillCamVictimHandle();
float CameraDirector_GetKillCamElapsed();
const CameraConfig& CameraDirector_GetConfig();

// World camera integration
// Returns: best camera index + type + score, or -1 if no good camera
struct WorldCamResult {
    int index;
    int type;  // 0=static, 1=dynamic
    float score;
};
// WorldCameraTracker provides this; CameraDirector calls it
WorldCamResult WorldCameraTracker_ScoreBestCamera(int* spectObj, float* playerPos);
```

### 5.2 CameraDirector.cpp — Core Logic Skeleton

```cpp
#include "CameraDirector.h"
#include "SpectatorController.h"  // for SetSpectatorToPlayerId()
#include "WorldCameraTracker.h"
#include "DelayManager.h"
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <algorithm>

// ============================================================================
// Internal State
// ============================================================================

static uintptr_t g_gameBase = 0;
static CameraConfig g_config;
static CameraState g_state = CameraState::Idle;
static std::mutex g_directorMutex;

// Current target
static int g_currentTargetHandle = 0;

// KillCam state
static KillCamPhase g_killCamPhase = KillCamPhase::Wait;
static int g_kcKillerHandle = 0;
static int g_kcVictimHandle = 0;
static DWORD g_kcStartTick = 0;
static float g_kcKillerPos[3] = {};
static float g_kcVictimPos[3] = {};

// Flag state
static int g_flagCarrierUS = 0;
static int g_flagCarrierVC = 0;
static int g_lastFlagTarget = 0;
static DWORD g_flagAlternateTimer = 0;

// Cooldowns
static DWORD g_lastKillSwitch = 0;
static DWORD g_flagLostTimestamp = 0;
static bool g_flagLostGraceActive = false;

// Event queue
static std::vector<CameraEvent> g_pendingEvents;

// ============================================================================
// Entity helpers (reuse from existing code)
// ============================================================================

static void* FindEntityByHandle(int handle) {
    void** playerTable = (void**)(g_gameBase + 0x7AE9C8);
    for (int i = 0; i < 64; i++) {
        if (!playerTable[i]) continue;
        if (*(int*)playerTable[i] == handle) {
            uintptr_t entity = *(uintptr_t*)((uintptr_t)playerTable[i] + 244);
            return entity ? (void*)entity : nullptr;
        }
    }
    return nullptr;
}

static void GetEntityPos(void* entity, float* out) {
    out[0] = *(float*)((uintptr_t)entity + 0xD0);
    out[1] = *(float*)((uintptr_t)entity + 0xD4);
    out[2] = *(float*)((uintptr_t)entity + 0xD8);
}

// ============================================================================
// Distance-Based Cinematic Chance
// ============================================================================

static float GetKillDistance(int killerHandle, int victimHandle) {
    void* killerEntity = FindEntityByHandle(killerHandle);
    void* victimEntity = FindEntityByHandle(victimHandle);
    if (!killerEntity || !victimEntity) return 0.0f;

    float kp[3], vp[3];
    GetEntityPos(killerEntity, kp);
    GetEntityPos(victimEntity, vp);

    float dx = kp[0] - vp[0];
    float dy = kp[1] - vp[1];
    float dz = kp[2] - vp[2];
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

static float ComputeCinematicChance(float distance) {
    if (distance < g_config.cinematicMinDistance) return 0.0f;
    if (distance >= g_config.cinematicMaxDistance) return g_config.cinematicMaxChance;

    float t = (distance - g_config.cinematicMinDistance)
            / (g_config.cinematicMaxDistance - g_config.cinematicMinDistance);
    return g_config.cinematicMinChance + t * (g_config.cinematicMaxChance - g_config.cinematicMinChance);
}

static bool RollCinematic(float chance) {
    float roll = (float)(rand() % 1000) / 1000.0f;
    return roll < chance;
}

// ============================================================================
// State Transitions
// ============================================================================

static void EnterFollowPlayer(int targetHandle) {
    g_state = CameraState::FollowPlayer;
    g_currentTargetHandle = targetHandle;
    SetSpectatorToPlayerId(targetHandle);
    // WorldCameraTracker will be queried each frame for ambient scoring
}

static void EnterKillCam(int killerHandle, int victimHandle) {
    g_state = CameraState::KillCam;
    g_killCamPhase = KillCamPhase::Wait;
    g_kcKillerHandle = killerHandle;
    g_kcVictimHandle = victimHandle;
    g_kcStartTick = GetTickCount();
    g_currentTargetHandle = killerHandle;

    // Snapshot positions
    void* ke = FindEntityByHandle(killerHandle);
    void* ve = FindEntityByHandle(victimHandle);
    if (ke) GetEntityPos(ke, g_kcKillerPos);
    if (ve) GetEntityPos(ve, g_kcVictimPos);

    SetSpectatorToPlayerId(killerHandle);

    std::cout << "[CameraDirector] KillCam started (killer=" << killerHandle
              << " victim=" << victimHandle << ")\n";
}

static void EnterFlagWatch(int carrierHandle) {
    g_state = CameraState::FlagWatch;
    g_currentTargetHandle = carrierHandle;
    SetSpectatorToPlayerId(carrierHandle);
}

static void ExitKillCam() {
    g_state = CameraState::FollowPlayer;
    g_kcKillerHandle = 0;
    g_kcVictimHandle = 0;
    std::cout << "[CameraDirector] KillCam ended\n";
}

// ============================================================================
// KillCam Phase Management
// ============================================================================

static float GetKillCamElapsed() {
    return (GetTickCount() - g_kcStartTick) / 1000.0f;
}

static float GetKillCamTotalDuration() {
    return g_config.killCamWaitDuration
         + g_config.killCamTransitionDuration
         + g_config.killCamAttachedDuration;
}

static bool IsKillCamInterruptible() {
    float elapsed = GetKillCamElapsed();
    float waitEnd = g_config.killCamWaitDuration;
    float transEnd = waitEnd + g_config.killCamTransitionDuration;

    if (elapsed < waitEnd) return true;              // Phase 0: interruptible
    if (elapsed < transEnd) return false;             // Phase 1: NOT interruptible
    if (elapsed > transEnd + 1.0f) return true;       // Phase 2 after 1s settle: yes
    return false;
}

static void UpdateKillCamPhase() {
    float elapsed = GetKillCamElapsed();
    float waitEnd = g_config.killCamWaitDuration;
    float transEnd = waitEnd + g_config.killCamTransitionDuration;
    float totalEnd = GetKillCamTotalDuration();

    if (elapsed < waitEnd) {
        g_killCamPhase = KillCamPhase::Wait;
    } else if (elapsed < transEnd) {
        g_killCamPhase = KillCamPhase::Transition;
    } else if (elapsed < totalEnd) {
        g_killCamPhase = KillCamPhase::Attached;
    } else {
        // Time's up
        ExitKillCam();
    }
}

// ============================================================================
// Event Processing
// ============================================================================

void CameraDirector_OnKill(int killerHandle, int victimHandle) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();

    // Respect cooldown
    if (now - g_lastKillSwitch < (DWORD)(g_config.killCooldown * 1000)) {
        return;
    }

    // Don't interrupt flag watching
    if (g_state == CameraState::FlagWatch) {
        return;
    }

    // Measure kill distance
    float distance = GetKillDistance(killerHandle, victimHandle);

    // Compute cinematic chance based on distance
    float chance = ComputeCinematicChance(distance);
    bool cinematic = RollCinematic(chance);

    std::cout << "[CameraDirector] Kill: killer=" << killerHandle
              << " victim=" << victimHandle
              << " distance=" << distance << "m"
              << " cinematicChance=" << (int)(chance * 100) << "%"
              << " result=" << (cinematic ? "CINEMATIC" : "NORMAL") << "\n";

    g_lastKillSwitch = now;

    if (cinematic) {
        EnterKillCam(killerHandle, victimHandle);
    } else {
        EnterFollowPlayer(killerHandle);
    }
}

void CameraDirector_OnFlagChanged(int usCarrier, int vcCarrier) {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    g_flagCarrierUS = usCarrier;
    g_flagCarrierVC = vcCarrier;

    bool anyFlag = (usCarrier != 0 || vcCarrier != 0);

    if (anyFlag) {
        g_flagLostGraceActive = false;

        // Pick which carrier to watch
        int target = 0;
        if (usCarrier != 0 && vcCarrier == 0) target = usCarrier;
        else if (vcCarrier != 0 && usCarrier == 0) target = vcCarrier;
        else {
            // Both flags taken — watch the one we're not currently on
            target = (g_currentTargetHandle == usCarrier) ? vcCarrier : usCarrier;
            g_flagAlternateTimer = GetTickCount();
        }

        // Only interrupt KillCam if it's interruptible
        if (g_state == CameraState::KillCam && !IsKillCamInterruptible()) {
            // Queue the flag event for after KillCam
            return;
        }

        g_lastFlagTarget = target;
        EnterFlagWatch(target);
    } else {
        // No flags — start grace period
        if (g_state == CameraState::FlagWatch && !g_flagLostGraceActive) {
            g_flagLostGraceActive = true;
            g_flagLostTimestamp = GetTickCount();
        }
    }
}

// ============================================================================
// Per-Frame Update
// ============================================================================

void CameraDirector_Update() {
    std::lock_guard<std::mutex> lock(g_directorMutex);

    DWORD now = GetTickCount();

    switch (g_state) {
        case CameraState::KillCam:
            UpdateKillCamPhase();
            break;

        case CameraState::FlagWatch: {
            // Handle flag lost grace period
            if (g_flagLostGraceActive) {
                float elapsed = (now - g_flagLostTimestamp) / 1000.0f;
                if (elapsed > g_config.flagLostGracePeriod) {
                    g_flagLostGraceActive = false;
                    g_state = CameraState::FollowPlayer;
                    std::cout << "[CameraDirector] Flag grace expired, returning to kills\n";
                    break;
                }
            }

            // Handle dual-flag alternation
            if (g_flagCarrierUS != 0 && g_flagCarrierVC != 0) {
                float elapsed = (now - g_flagAlternateTimer) / 1000.0f;
                if (elapsed > 5.0f) {
                    // Switch to the other carrier
                    int nextTarget = (g_currentTargetHandle == g_flagCarrierUS)
                                   ? g_flagCarrierVC : g_flagCarrierUS;
                    g_currentTargetHandle = nextTarget;
                    SetSpectatorToPlayerId(nextTarget);
                    g_flagAlternateTimer = now;
                }
            }
            break;
        }

        case CameraState::FollowPlayer:
        case CameraState::Idle:
            // WorldCam ambient scoring happens here.
            // CameraDirector decides whether to briefly switch to a world cam
            // for cinematic variety. This replaces the autonomous WorldCameraTracker.
            //
            // (See section 5.4 for world camera integration)
            break;

        default:
            break;
    }
}

// ============================================================================
// State Accessors (for FirstPersonCamera hook)
// ============================================================================

CameraState CameraDirector_GetState() { return g_state; }
KillCamPhase CameraDirector_GetKillCamPhase() { return g_killCamPhase; }
int CameraDirector_GetTargetHandle() { return g_currentTargetHandle; }
int CameraDirector_GetKillCamKillerHandle() { return g_kcKillerHandle; }
int CameraDirector_GetKillCamVictimHandle() { return g_kcVictimHandle; }
float CameraDirector_GetKillCamElapsed() { return GetKillCamElapsed(); }
const CameraConfig& CameraDirector_GetConfig() { return g_config; }

// ============================================================================
// Lifecycle
// ============================================================================

void InitCameraDirector(uintptr_t gameBase) {
    g_gameBase = gameBase;
    srand((unsigned int)time(nullptr));

    // Config is loaded via DelayManager::Init() which reads the INI
    // CameraDirector reads its own section (see INI changes below)
    LoadCameraDirectorConfig(g_config);  // new function in DelayManager

    std::cout << "[CameraDirector] Initialized\n";
    std::cout << "[CameraDirector] Cinematic range: "
              << g_config.cinematicMinDistance << "m (" << (int)(g_config.cinematicMinChance*100) << "%) → "
              << g_config.cinematicMaxDistance << "m (" << (int)(g_config.cinematicMaxChance*100) << "%)\n";
}
```

### 5.3 Changes to FirstPersonCamera.cpp — Hooked_FillCamera

The hook function becomes a pure **executor** — it reads state from CameraDirector and renders accordingly. No decision-making.

```cpp
// BEFORE (current):
//   - Manages g_currentCameraMode, g_killCamPhase internally
//   - Has its own KillCam state machine
//   - OnPlayerChanged() resets local state

// AFTER (new):
//   - Reads CameraDirector_GetState(), CameraDirector_GetKillCamPhase()
//   - Executes 3PV smoothing, KillCam slide math based on director state
//   - No local camera mode tracking

int __fastcall Hooked_FillCamera(void* thisPtr, void* edx_unused, void* cameraProp) {
    DelayManager::ReloadConfigIfNeeded();

    int mode = *(int*)((char*)thisPtr + SPECTATOR_CTRL_MODE);
    if (mode != SPECTATOR_MODE_FREE) {
        return g_OriginalFillCamera(thisPtr, cameraProp);
    }

    CameraState directorState = CameraDirector_GetState();
    const CameraConfig& cfg = CameraDirector_GetConfig();

    // --- Pre-processing: yaw/pitch from bone rotation ---
    // Run for all states EXCEPT KillCam Transition phase
    bool doPreprocess = !(directorState == CameraState::KillCam
                         && CameraDirector_GetKillCamPhase() == KillCamPhase::Transition);

    if (doPreprocess) {
        // ... existing 3PV bone rotation + yaw smoothing code ...
        // (identical to current implementation, just controlled by director state)
    }

    // --- Call original ---
    int result = g_OriginalFillCamera(thisPtr, cameraProp);

    // --- Post-processing: distance smoothing ---
    bool doPostprocess = doPreprocess;  // same condition
    if (doPostprocess && result) {
        // ... existing distance clamping code ...
    }

    // --- KillCam phase execution ---
    if (directorState == CameraState::KillCam && result) {
        KillCamPhase phase = CameraDirector_GetKillCamPhase();
        float elapsed = CameraDirector_GetKillCamElapsed();

        // Phase 0 (Wait): normal 3PV already handled above

        // Phase 1 (Transition): slide camera
        if (phase == KillCamPhase::Transition) {
            // ... existing slide interpolation math ...
            // Uses cfg.killCamWaitDuration, cfg.killCamTransitionDuration
        }

        // Phase 2 (Attached): switch to victim on first frame
        if (phase == KillCamPhase::Attached) {
            // ... existing victim switch + 3PV reset ...
        }
    }

    return result;
}
```

### 5.4 World Camera Integration

`WorldCameraTracker.cpp` becomes a passive scoring service:

```cpp
// NEW API: Score cameras without switching
WorldCamResult WorldCameraTracker_ScoreBestCamera(int* spectObj, float* playerPos) {
    // Same FindBestCamera() logic as today, but returns result
    // instead of modifying spectObj directly.
    CamSearchResult result = FindBestCamera(spectObj, playerPos);
    return { result.index, result.type, result.score };
}

// CameraDirector calls this periodically during FollowPlayer state
// and decides whether to switch to world cam for ambient variety:
void CameraDirector_EvaluateWorldCam() {
    // Only during FollowPlayer, not during KillCam or FlagWatch
    if (g_state != CameraState::FollowPlayer) return;

    // Get player position, call WorldCameraTracker_ScoreBestCamera()
    // If score > threshold, switch to world cam for variety
    // Apply cooldown and max hold time from config
}
```

### 5.5 Changes to DelayManager — Routing to CameraDirector

```cpp
// In DelayManager::AddDelayedAction() and ProcessActions():
// Replace:
//   ProcessKillEvent(action.killerId, action.victimId);
//   ProcessFlagEvent(action.usCarrier, action.vcCarrier);
// With:
//   CameraDirector_OnKill(action.killerId, action.victimId);
//   CameraDirector_OnFlagChanged(action.usCarrier, action.vcCarrier);
```

### 5.6 New INI Configuration Keys

Add these to `vcstreamer.ini` (alongside existing keys):

```ini
; ============================================
; Camera Director Configuration
; ============================================

; Distance-based cinematic kill camera
; Below min_distance: 0% cinematic chance
; At min_distance: min_chance cinematic
; At max_distance+: max_chance cinematic
; Linear interpolation between min and max
cinematic_min_distance=25.0
cinematic_max_distance=50.0
cinematic_min_chance=30
cinematic_max_chance=70

; KillCam phase durations (seconds)
killcam_wait_duration=3.5
killcam_transition_duration=1.5
killcam_attached_duration=4.0
killcam_slide_height=0.8

; Cooldowns (seconds)
kill_cooldown=5.0
flag_lost_grace=3.0

; World camera settings
worldcam_max_distance=18.0
worldcam_switch_cooldown=4.0
worldcam_max_hold=10.0
worldcam_los_penalty=60.0
worldcam_stickiness=5.0

; 3PV smoothing
tpv_max_distance=2.5
tpv_yaw_smooth=0.01
tpv_zoom_in_factor=0.3
tpv_zoom_out_factor=0.005
```

---

## 6. Initialization Sequence Changes

### Current (dllmain.cpp):
```
InitSpectatorController(base)
GameMemoryReader::Init(base)
DelayManager::SetGameBase(base) + Init()
InitTickDelayBuffer(base)
InitFirstPersonCamera(base)
InitRealtimeHook(base)
InitFlagMonitor(base)          ← REMOVE
InitAutoSpectator(base)
InitWorldCameraTracker(base)
```

### New:
```
InitSpectatorController(base)    // simplified: just SetSpectatorToPlayerId + PatchCameraDistance
GameMemoryReader::Init(base)
DelayManager::SetGameBase(base) + Init()
InitTickDelayBuffer(base)
InitCameraDirector(base)         ← NEW (before FirstPersonCamera, so config is ready)
InitFirstPersonCamera(base)
InitRealtimeHook(base)
// InitFlagMonitor(base)         ← REMOVED (network-based flag detection only)
InitAutoSpectator(base)
InitWorldCameraTracker(base)     // simplified: passive scorer only
```

### Main Loop changes:
```
// Current:
DelayManager::ProcessActions()     // fires ProcessKillEvent, ProcessFlagEvent
GameMemoryReader::ReadPlayerList()
UpdateCameraOrientation()

// New:
DelayManager::ProcessActions()     // fires CameraDirector_OnKill, CameraDirector_OnFlagChanged
GameMemoryReader::ReadPlayerList()
CameraDirector_Update()            // NEW: per-frame director tick (replaces UpdateCameraOrientation)
```

---

## 7. Migration Checklist

The implementation should be done in this order to minimize risk:

### Phase 1: Create CameraDirector (additive, no breakage)
- [ ] Create `CameraDirector.h` with full API, enums, config struct
- [ ] Create `CameraDirector.cpp` with all logic (OnKill, OnFlagChanged, Update, state machine, distance formula)
- [ ] Add new INI keys to `DelayManager.cpp` config loader
- [ ] Add `InitCameraDirector(base)` to `dllmain.cpp` startup
- [ ] Add `CameraDirector_Update()` to main loop

### Phase 2: Route events to CameraDirector
- [ ] Change `DelayManager::AddDelayedAction()` and `ProcessActions()` to call `CameraDirector_OnKill` / `CameraDirector_OnFlagChanged` instead of `ProcessKillEvent` / `ProcessFlagEvent`
- [ ] Verify events flow correctly (debug logging)

### Phase 3: Simplify FirstPersonCamera
- [ ] Remove local `g_currentCameraMode`, `g_killCamPhase` state management
- [ ] Read from `CameraDirector_GetState()`, `CameraDirector_GetKillCamPhase()` instead
- [ ] Remove `StartKillCam()` public function (CameraDirector handles this)
- [ ] Keep all camera math (smoothing, interpolation) intact — just change what drives it

### Phase 4: Simplify WorldCameraTracker
- [ ] Convert `WorldCameraTracker_Update()` to passive `WorldCameraTracker_ScoreBestCamera()`
- [ ] Remove autonomous mode switching (spectObj writes)
- [ ] CameraDirector calls scorer and decides when to use world cams

### Phase 5: Simplify SpectatorController
- [ ] Remove `ProcessKillEvent()`, `ProcessFlagEvent()`, flag watcher thread, cooldown logic
- [ ] Keep `SetSpectatorToPlayerId()`, `UpdateScoreboard()`, `PatchCameraDistance()`
- [ ] Remove `SetCameraOrientation()` and `UpdateCameraOrientation()` (dead code)

### Phase 6: Remove FlagMonitor
- [ ] Delete `FlagMonitor.cpp` and `FlagMonitor.h`
- [ ] Remove `InitFlagMonitor(base)` from `dllmain.cpp`
- [ ] Verify network-based flag detection (RealtimeHook) covers all cases

### Phase 7: Test and tune
- [ ] Test close-range kills (< 25m): should never trigger cinematic
- [ ] Test medium-range kills (25–50m): should trigger cinematic ~30–70% based on distance
- [ ] Test long-range kills (> 50m): should trigger cinematic ~70% of the time
- [ ] Test flag grab interrupts KillCam correctly (only during safe phases)
- [ ] Test world camera variety during idle periods
- [ ] Tune INI parameters based on live streaming feedback

---

## 8. Thread Safety Notes

The current codebase has multiple threading concerns. The CameraDirector addresses them:

- **Single mutex (`g_directorMutex`)** protects all director state. No more scattered atomics mixed with unprotected variables.
- **Event input functions** (`OnKill`, `OnFlagChanged`) acquire the lock, make decisions, and release. Short critical sections.
- **Per-frame Update** acquires the lock for state machine ticking. Also short.
- **State accessor functions** (`GetState`, `GetKillCamPhase`, etc.) return simple values. For `Hooked_FillCamera` which runs at render framerate, these should be lock-free reads of atomic-compatible state. Use `std::atomic` for the state enum and phase enum to avoid locking the hook.

**Recommended pattern:**
```cpp
// CameraDirector internal: protected by mutex
static std::atomic<CameraState> g_state_atomic;
static std::atomic<KillCamPhase> g_phase_atomic;

// After any state change inside locked section:
g_state_atomic.store(g_state, std::memory_order_release);
g_phase_atomic.store(g_killCamPhase, std::memory_order_release);

// Hooked_FillCamera reads without locking:
CameraState state = g_state_atomic.load(std::memory_order_acquire);
```

---

## 9. Cinematic Best Practices Applied

Based on research into game camera systems, esports broadcasting, and cinematography:

| Principle | How We Apply It |
|-----------|----------------|
| **"The invisible camera"** — best camera work goes unnoticed | Smooth transitions, no jarring cuts. SmoothStep interpolation during KillCam slide. Yaw lerping in 3PV. |
| **Trigger cinematics during low-action moments** | KillCam only triggers after a kill (combat resolved). Never interrupts active gameplay. |
| **Never cut during a transition** | KillCam Phase 1 (slide) is marked non-interruptible. Even high-priority flag events wait. |
| **Distance creates drama** | Long-range kills get cinematic treatment because the distance itself tells a story — the shot traveled far, the viewer wants to see both perspectives. |
| **Variety prevents predictability** | Max 70% cinematic chance even at extreme range. World cam variety switches. Flag alternation. |
| **Decay-based priority** | Old events naturally lose importance. A 5-second-old kill shouldn't override a fresh flag grab. |
| **Ease-in/ease-out transitions** | SmoothStep `t*t*(3-2t)` for KillCam slide. Asymmetric zoom factors (fast zoom-in, slow zoom-out) for 3PV distance. |
| **Frame rate independent** | All timing uses `GetTickCount()` milliseconds, not frame counts. World cam cooldowns should also convert to time-based. |
| **Configurable everything** | All magic numbers become INI parameters. Live-tunable in debug mode. |

---

## 10. Summary of What Changes

| Before | After |
|--------|-------|
| Camera decisions in 6 files | Camera decisions in 1 file (CameraDirector) |
| Flat 50% KillCam chance | Distance-based 0–70% cinematic chance |
| No distance awareness | Full kill distance measurement and scaling |
| Dual flag tracking (memory + network) | Single source: network packets only |
| Autonomous world camera switching | Passive scoring, director decides |
| Hardcoded constants everywhere | Central INI configuration |
| Mixed threading models | Single mutex + atomic state for hook |
| Flag watcher runs its own thread | Director handles flag state in Update() |
| 6 files to understand camera flow | 1 file to understand all decisions |

**Lines of code estimate:**
- New: ~400 lines (CameraDirector.cpp + .h)
- Removed: ~200 lines (ProcessKillEvent, ProcessFlagEvent, FlagMonitor, autonomous WorldCam logic)
- Modified: ~100 lines (FirstPersonCamera hook, DelayManager routing, dllmain init)
- Net change: ~+100 lines, dramatically simpler architecture
