#define _CRT_SECURE_NO_WARNINGS
#include "CommentaryEngine.h"
#include "DelayManager.h"
#include "DiagnosticsLog.h"
#include "PlayerIdentityCache.h"
#include "ServerTelemetry.h"
#include "SpectatorController.h"
#include "TickDelayBuffer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr DWORD kRapidComboWindowMs = 5500;
constexpr DWORD kGlobalKillCooldownMs = 500;
constexpr DWORD kPlayerKillCooldownMs = 900;
constexpr DWORD kGlobalLineGapMs = 1200;
constexpr DWORD kActiveHoldMs = 3600;
constexpr DWORD kFadeInMs = 180;
constexpr DWORD kFadeOutMs = 900;
constexpr DWORD kLateKillDropMs = 1500;
constexpr DWORD kLateFlagDropMs = 2500;
constexpr DWORD kStatStableTargetMs = 2000;
constexpr DWORD kStatMinIntervalMs = 4000;
constexpr DWORD kPostKillDeathWindowMs = 7000;
constexpr DWORD kLiveTargetEventFreshMs = 9000;
constexpr DWORD kScoreCommentMinDelayMs = 3000;
constexpr DWORD kScoreCommentMaxDelayMs = 8000;
constexpr DWORD kScoreCommentMinIntervalMs = 20000;
constexpr DWORD kStableScoreMinIntervalMs = 120000;
constexpr DWORD kStableScoreMaxIntervalMs = 240000;
constexpr DWORD kAggregateStatMinIntervalMs = 60000;
constexpr DWORD kAggregateStatMaxIntervalMs = 120000;
constexpr size_t kTargetBacklogMax = 64;
constexpr size_t kRecentGeneralTemplateCount = 5;
constexpr unsigned int kHitFlagKill = 1u << 0;

struct ActiveLine {
    bool active = false;
    DWORD startTick = 0;
    DWORD expireTick = 0;
    DWORD activationTick = 0;
    int requiredTarget = 0;
    int priority = 0;
    int subjectHandle = 0;
    int objectHandle = 0;
    unsigned long long eventSequence = 0;
    bool cameraSwitchTolerant = false;
    std::string category;
    std::string nameSource;
    std::string templateKey;
    std::string text;
};

struct PendingLine {
    bool pending = false;
    DWORD activationTick = 0;
    DWORD expireTick = 0;
    int requiredTarget = 0;
    int priority = 0;
    int subjectHandle = 0;
    int objectHandle = 0;
    unsigned long long eventSequence = 0;
    bool cameraSwitchTolerant = false;
    std::string category;
    std::string nameSource;
    std::string templateKey;
    std::string text;
};

struct LastCommentedKill {
    bool valid = false;
    int killerHandle = 0;
    int victimHandle = 0;
    DWORD lineTick = 0;
    unsigned long long eventSequence = 0;
    char killerName[48] = {};
    char killerNameSource[32] = {};
};

struct LocalPlayerState {
    int rapidKills = 0;
    DWORD lastRapidKillMs = 0;
};

struct ResolvedName {
    bool ok = false;
    bool roleFallback = false;
    char name[48] = {};
    const char* source = "none";
};

struct TeamScoreState {
    bool valid = false;
    int us = 0;
    int vc = 0;
};

struct PendingScoreChange {
    bool pending = false;
    DWORD changeTick = 0;
    DWORD eligibleTick = 0;
    DWORD expireTick = 0;
    int oldUs = 0;
    int oldVc = 0;
    int newUs = 0;
    int newVc = 0;
};

struct TeamAggregateStats {
    unsigned long long totalDamage = 0;
    unsigned long long distanceMeters = 0;
    unsigned long long flagDistanceMeters = 0;
    unsigned long long flagAttempts = 0;
    unsigned long long accuracyShots = 0;
    unsigned long long accuracyHits = 0;
    unsigned long long teamkills = 0;
    unsigned long long aliveTimeSec = 0;
    unsigned int players = 0;
};

struct GeneralCommentCandidate {
    std::string category;
    std::string templateKey;
    std::string text;
};

std::mutex g_mutex;
ActiveLine g_active;
PendingLine g_pending;
std::vector<PendingLine> g_targetBacklog;
DWORD g_lastGlobalKillLine = 0;
DWORD g_lastGlobalLineStart = 0;
DWORD g_nextStatTick = 0;
DWORD g_nextStableScoreTick = 0;
DWORD g_nextAggregateStatTick = 0;
DWORD g_lastScoreCommentTick = 0;
DWORD g_targetSinceTick = 0;
int g_lastContextTarget = 0;
unsigned long long g_generalSequence = 1;
TeamScoreState g_teamScore;
PendingScoreChange g_pendingScoreChange;
std::unordered_map<int, DWORD> g_lastPlayerKillLine;
std::unordered_map<int, DWORD> g_nextFlagLoopTick;
std::unordered_map<int, unsigned int> g_statCursor;
std::unordered_map<int, LocalPlayerState> g_playerStates;
std::unordered_map<int, ServerTelemetryStatsDelta> g_latestStats;
std::unordered_map<int, unsigned int> g_localKills;
std::unordered_map<int, unsigned int> g_localDeaths;
std::unordered_set<unsigned long long> g_seenKillSequences;
std::unordered_set<unsigned long long> g_liveKillLineSequences;
std::unordered_set<unsigned long long> g_liveHitLineSequences;
std::unordered_set<unsigned long long> g_liveFlagLineSequences;
std::unordered_set<unsigned long long> g_liveAchievementLineSequences;
std::deque<std::string> g_recentGeneralTemplates;
std::string g_lastGeneralCategory;
DWORD g_lastTelemetryCacheTick = 0;
LastCommentedKill g_lastCommentedKill;

CommentaryCameraContext g_lastContext;

using VchdSetCommentaryLineFn = void(__cdecl*)(const char*, float);
HMODULE g_vchdModule = nullptr;
VchdSetCommentaryLineFn g_setVchdCommentaryLine = nullptr;
bool g_vchdResolveAttempted = false;
bool g_lastPublishedVisible = false;

void CommentaryLog(const char* fmt, ...) {
    FILE* file = fopen("commentary_debug.log", "a");
    if (!file) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fclose(file);
}

bool ResolveVchdCommentaryExport() {
    HMODULE module = GetModuleHandleA("dinput8.dll");
    if (!module) {
        module = GetModuleHandleA("vchd_dinput8.dll");
    }
    if (!module) return false;
    if (g_vchdModule == module && g_setVchdCommentaryLine) return true;

    g_vchdModule = module;
    g_setVchdCommentaryLine = reinterpret_cast<VchdSetCommentaryLineFn>(
        GetProcAddress(module, "VCHD_StreamerSetCommentaryLine"));
    if (!g_setVchdCommentaryLine && !g_vchdResolveAttempted) {
        CommentaryLog("VCHD commentary export missing\n");
    }
    g_vchdResolveAttempted = true;
    return g_setVchdCommentaryLine != nullptr;
}

bool TickReached(DWORD now, DWORD tick) {
    return static_cast<LONG>(now - tick) >= 0;
}

bool IsTargetStillActive(int requiredTarget, int activeCameraTarget) {
    return requiredTarget == 0 || requiredTarget == activeCameraTarget;
}

bool IsLineTargetStillActive(const PendingLine& line, int activeCameraTarget) {
    return line.cameraSwitchTolerant || IsTargetStillActive(line.requiredTarget, activeCameraTarget);
}

bool IsLineTargetStillActive(const ActiveLine& line, int activeCameraTarget) {
    return line.cameraSwitchTolerant || IsTargetStillActive(line.requiredTarget, activeCameraTarget);
}

DWORD RandomDelay(DWORD seed, DWORD minMs, DWORD maxMs) {
    if (maxMs <= minMs) return minMs;
    DWORD span = maxMs - minMs + 1;
    DWORD value = seed * 1103515245u + 12345u;
    return minMs + (value % span);
}

bool IsFlagShot(const char* shotKind) {
    return shotKind && strcmp(shotKind, "flag") == 0;
}

const char* TeamName(int team) {
    if (team == 0) return "US";
    if (team == 1) return "VC";
    return "team";
}

void CopyText(char* out, size_t outLen, const char* text) {
    if (!out || outLen == 0) return;
    strncpy_s(out, outLen, text ? text : "", _TRUNCATE);
}

void FormatRoleLabel(char* out, size_t outLen, int team, const char* role, const char* fallback) {
    if (!out || outLen == 0) return;
    if (team == 0 || team == 1) {
        sprintf_s(out, outLen, "%s %s", TeamName(team), role && role[0] ? role : "player");
    } else {
        CopyText(out, outLen, fallback && fallback[0] ? fallback : "watched player");
    }
}

int IdentityTeamForHandle(int handle) {
    PlayerIdentity identity = {};
    if (PlayerIdentityCache_Resolve(handle, &identity)) {
        if (identity.team == 0 || identity.team == 1) {
            return identity.team;
        }
    }
    return -1;
}

ResolvedName ResolveNameWithRoleFallback(int handle,
                                         const char* telemetryName,
                                         const char* telemetrySource,
                                         int team,
                                         const char* role,
                                         const char* fallbackLabel) {
    ResolvedName resolved = {};
    if (PlayerIdentityCache_IsResolvedName(telemetryName)) {
        CopyText(resolved.name, sizeof(resolved.name), telemetryName);
        resolved.ok = true;
        resolved.source = telemetrySource && telemetrySource[0] ? telemetrySource : "telemetry";
        return resolved;
    }

    PlayerIdentity identity = {};
    if (PlayerIdentityCache_Resolve(handle, &identity)) {
        if (PlayerIdentityCache_IsResolvedName(identity.name)) {
            CopyText(resolved.name, sizeof(resolved.name), identity.name);
            resolved.ok = true;
            resolved.source = "identity-cache";
            return resolved;
        }
        if (team != 0 && team != 1 && (identity.team == 0 || identity.team == 1)) {
            team = identity.team;
        }
    }

    FormatRoleLabel(resolved.name, sizeof(resolved.name), team, role, fallbackLabel);
    resolved.ok = resolved.name[0] != '\0';
    resolved.roleFallback = true;
    resolved.source = "role-label";
    return resolved;
}

int KnownCarrierTeam(int handle) {
    if (handle != 0 && handle == g_lastContext.usCarrier) return 0;
    if (handle != 0 && handle == g_lastContext.vcCarrier) return 1;
    return -1;
}

bool IsKnownFlagCarrier(int handle) {
    return handle != 0
        && (handle == g_lastContext.usCarrier
            || handle == g_lastContext.vcCarrier
            || handle == g_lastContext.flagKillLookVictimHandle);
}

float ActiveAlphaLocked(DWORD now) {
    if (!g_active.active) return 0.0f;

    DWORD elapsed = now - g_active.startTick;
    float alpha = 1.0f;
    if (elapsed < kFadeInMs) {
        alpha = static_cast<float>(elapsed) / static_cast<float>(kFadeInMs);
    } else if (elapsed > kFadeInMs + kActiveHoldMs) {
        DWORD fadeElapsed = elapsed - kFadeInMs - kActiveHoldMs;
        alpha = 1.0f - static_cast<float>(fadeElapsed) / static_cast<float>(kFadeOutMs);
    }
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return alpha;
}

void PublishToVchdLocked(DWORD now) {
    if (!ResolveVchdCommentaryExport()) return;

    float alpha = ActiveAlphaLocked(now);
    if (g_active.active && alpha > 0.001f) {
        g_setVchdCommentaryLine(g_active.text.c_str(), alpha);
        g_lastPublishedVisible = true;
    } else if (g_lastPublishedVisible) {
        g_setVchdCommentaryLine("", 0.0f);
        g_lastPublishedVisible = false;
    }
}

void ClearActiveLocked(DWORD now, const char* reason) {
    if (g_active.active) {
        CommentaryLog("clear-active seq=%llu category=%s priority=%d target=%d cameraTarget=%d source=%s now=%lu reason=%s text=%s\n",
                      g_active.eventSequence,
                      g_active.category.c_str(),
                      g_active.priority,
                      g_active.requiredTarget,
                      g_lastContext.activeCameraTarget,
                      g_active.nameSource.c_str(),
                      now,
                      reason ? reason : "none",
                      g_active.text.c_str());
    }
    g_active = ActiveLine();
}

bool CanStartLineLocked(DWORD now, int priority) {
    return priority >= 90
        || g_lastGlobalLineStart == 0
        || static_cast<int>(now - g_lastGlobalLineStart) >= static_cast<int>(kGlobalLineGapMs);
}

bool IsFreshEvent(DWORD now, DWORD eventTick, DWORD freshMs) {
    if (eventTick == 0) return true;
    return TickReached(now, eventTick)
        && !TickReached(now, eventTick + freshMs);
}

DWORD PlaybackDelayMs() {
    int gameDelaySec = DelayManager::GetGameDelaySeconds();
    if (gameDelaySec < 0) gameDelaySec = 0;
    int localDelayMs = GetTickDelayMs();
    if (localDelayMs < 0) localDelayMs = 0;
    return static_cast<DWORD>(gameDelaySec * 1000 + localDelayMs);
}

DWORD LiveTelemetryBaseTick(DWORD receivedTick, DWORD eventTick, DWORD now) {
    if (receivedTick != 0) return receivedTick;
    if (eventTick != 0) return eventTick;
    return now;
}

DWORD LiveTelemetryActivationTick(DWORD receivedTick, DWORD eventTick, DWORD now) {
    DWORD baseTick = LiveTelemetryBaseTick(receivedTick, eventTick, now);
    return baseTick + PlaybackDelayMs();
}

DWORD LiveTelemetryVisibleEventTick(DWORD receivedTick, DWORD eventTick, DWORD now) {
    DWORD baseTick = eventTick != 0 ? eventTick : LiveTelemetryBaseTick(receivedTick, eventTick, now);
    return baseTick + PlaybackDelayMs();
}

bool HappenedOnCurrentTargetView(DWORD eventTick) {
    if (eventTick == 0 || g_targetSinceTick == 0) return true;
    return !TickReached(g_targetSinceTick, eventTick + 500);
}

bool IsTargetBacklogCategory(const std::string& category) {
    return category.find("target-") == 0;
}

bool IsGeneralGameStateCategory(const std::string& category) {
    return category.find("game-") == 0;
}

bool HasWatchedPlayerEventBacklogLocked() {
    if (g_pending.pending
        && (IsTargetBacklogCategory(g_pending.category) || g_pending.priority >= 85)) {
        return true;
    }
    return !g_targetBacklog.empty();
}

bool CanUseGeneralTemplate(const std::string& templateKey) {
    return std::find(g_recentGeneralTemplates.begin(),
                     g_recentGeneralTemplates.end(),
                     templateKey) == g_recentGeneralTemplates.end();
}

void MarkGeneralTemplateUsedLocked(const PendingLine& line) {
    if (!IsGeneralGameStateCategory(line.category)) return;
    g_lastGeneralCategory = line.category;
    if (!line.templateKey.empty()) {
        g_recentGeneralTemplates.push_back(line.templateKey);
        while (g_recentGeneralTemplates.size() > kRecentGeneralTemplateCount) {
            g_recentGeneralTemplates.pop_front();
        }
    }
}

void MarkLineStartedLocked(const PendingLine& line, DWORD now) {
    g_lastGlobalLineStart = now;
    MarkGeneralTemplateUsedLocked(line);
    if (line.category.find("kill") != std::string::npos) {
        g_lastGlobalKillLine = now;
        if (line.subjectHandle != 0) {
            g_lastPlayerKillLine[line.subjectHandle] = now;
        }
        if (line.subjectHandle != 0
            && line.objectHandle != 0
            && line.category != "post-kill-death"
            && line.category != "suicide") {
            g_lastCommentedKill.valid = true;
            g_lastCommentedKill.killerHandle = line.subjectHandle;
            g_lastCommentedKill.victimHandle = line.objectHandle;
            g_lastCommentedKill.lineTick = now;
            g_lastCommentedKill.eventSequence = line.eventSequence;
            CopyText(g_lastCommentedKill.killerName,
                     sizeof(g_lastCommentedKill.killerName),
                     line.text.c_str());
            CopyText(g_lastCommentedKill.killerNameSource,
                     sizeof(g_lastCommentedKill.killerNameSource),
                     line.nameSource.c_str());
        }
    }
}

void QueueTargetBacklogLocked(const PendingLine& line, DWORD now, const char* reason) {
    (void)now;
    if (line.text.empty()) return;
    for (const PendingLine& existing : g_targetBacklog) {
        if (existing.eventSequence == line.eventSequence
            && existing.category == line.category) {
            return;
        }
    }

    if (g_targetBacklog.size() >= kTargetBacklogMax) {
        CommentaryLog("drop-backlog seq=%llu category=%s target=%d reason=target-backlog-full text=%s\n",
                      g_targetBacklog.front().eventSequence,
                      g_targetBacklog.front().category.c_str(),
                      g_targetBacklog.front().requiredTarget,
                      g_targetBacklog.front().text.c_str());
        g_targetBacklog.erase(g_targetBacklog.begin());
    }

    auto insertAt = std::find_if(g_targetBacklog.begin(),
                                 g_targetBacklog.end(),
                                 [&line](const PendingLine& existing) {
                                     if (TickReached(existing.activationTick, line.activationTick)
                                         && existing.activationTick != line.activationTick) {
                                         return true;
                                     }
                                     return existing.activationTick == line.activationTick
                                         && existing.eventSequence > line.eventSequence;
                                 });
    g_targetBacklog.insert(insertAt, line);
    CommentaryLog("backlog seq=%llu category=%s activation=%lu expire=%lu target=%d cameraTarget=%d priority=%d source=%s reason=%s text=%s\n",
                  line.eventSequence,
                  line.category.c_str(),
                  line.activationTick,
                  line.expireTick,
                  line.requiredTarget,
                  g_lastContext.activeCameraTarget,
                  line.priority,
                  line.nameSource.c_str(),
                  reason ? reason : "target-event",
                  line.text.c_str());
}

void ActivatePendingLocked(const PendingLine& line,
                           DWORD now,
                           int activeCameraTarget,
                           const char* shotKind) {
    g_active.active = true;
    g_active.startTick = now;
    g_active.expireTick = now + kFadeInMs + kActiveHoldMs + kFadeOutMs;
    g_active.activationTick = line.activationTick;
    g_active.requiredTarget = line.requiredTarget;
    g_active.priority = line.priority;
    g_active.subjectHandle = line.subjectHandle;
    g_active.objectHandle = line.objectHandle;
    g_active.eventSequence = line.eventSequence;
    g_active.cameraSwitchTolerant = line.cameraSwitchTolerant;
    g_active.category = line.category;
    g_active.nameSource = line.nameSource;
    g_active.templateKey = line.templateKey;
    g_active.text = line.text;

    MarkLineStartedLocked(line, now);
    CommentaryLog("active seq=%llu category=%s priority=%d target=%d cameraTarget=%d source=%s activation=%lu now=%lu shot=%s text=%s\n",
                  g_active.eventSequence,
                  g_active.category.c_str(),
                  g_active.priority,
                  g_active.requiredTarget,
                  activeCameraTarget,
                  g_active.nameSource.c_str(),
                  g_active.activationTick,
                  now,
                  shotKind ? shotKind : "unknown",
                  g_active.text.c_str());
    DiagnosticsLog_Append("commentary_debug.log",
                          "[Commentary] category=%s priority=%d cameraTarget=%d source=%s text=%s\n",
                          g_active.category.c_str(),
                          g_active.priority,
                          activeCameraTarget,
                          g_active.nameSource.c_str(),
                          g_active.text.c_str());
}

void SetPendingLocked(const PendingLine& line, DWORD now, const char* reason) {
    if (line.text.empty()) return;

    if (g_active.active) {
        if (IsTargetBacklogCategory(line.category)
            && line.priority < 96) {
            QueueTargetBacklogLocked(line, now, reason);
            return;
        }
        if (line.priority >= 90 && line.priority >= g_active.priority) {
            ClearActiveLocked(now, "major-event-replace");
        } else if (IsTargetBacklogCategory(line.category)
                   && line.priority >= 85) {
            QueueTargetBacklogLocked(line, now, reason);
            return;
        } else {
            CommentaryLog("drop seq=%llu category=%s priority=%d activePriority=%d cameraTarget=%d source=%s reason=active-line text=%s\n",
                          line.eventSequence,
                          line.category.c_str(),
                          line.priority,
                          g_active.priority,
                          g_lastContext.activeCameraTarget,
                          line.nameSource.c_str(),
                          line.text.c_str());
            return;
        }
    }

    if (g_pending.pending && line.priority < g_pending.priority) {
        if (IsTargetBacklogCategory(line.category)
            && line.priority >= 85) {
            QueueTargetBacklogLocked(line, now, reason);
            return;
        }
        CommentaryLog("drop seq=%llu category=%s priority=%d pendingPriority=%d cameraTarget=%d source=%s reason=weaker-pending text=%s\n",
                      line.eventSequence,
                      line.category.c_str(),
                      line.priority,
                      g_pending.priority,
                      g_lastContext.activeCameraTarget,
                      line.nameSource.c_str(),
                      line.text.c_str());
        return;
    }

    g_pending = line;
    CommentaryLog("pending seq=%llu category=%s activation=%lu expire=%lu target=%d cameraTarget=%d priority=%d source=%s reason=%s text=%s\n",
                  line.eventSequence,
                  line.category.c_str(),
                  line.activationTick,
                  line.expireTick,
                  line.requiredTarget,
                  g_lastContext.activeCameraTarget,
                  line.priority,
                  line.nameSource.c_str(),
                  reason ? reason : "commit",
                  line.text.c_str());
}

void PromoteTargetBacklogLocked(DWORD now, int activeCameraTarget, const char* shotKind) {
    for (size_t i = 0; i < g_targetBacklog.size();) {
        PendingLine& line = g_targetBacklog[i];
        if (!IsLineTargetStillActive(line, activeCameraTarget)) {
            CommentaryLog("drop-backlog seq=%llu category=%s requiredTarget=%d cameraTarget=%d reason=off-camera text=%s\n",
                          line.eventSequence,
                          line.category.c_str(),
                          line.requiredTarget,
                          activeCameraTarget,
                          line.text.c_str());
            g_targetBacklog.erase(g_targetBacklog.begin() + i);
        } else if (TickReached(now, line.expireTick)) {
            CommentaryLog("drop-backlog seq=%llu category=%s activation=%lu expire=%lu now=%lu reason=late text=%s\n",
                          line.eventSequence,
                          line.category.c_str(),
                          line.activationTick,
                          line.expireTick,
                          now,
                          line.text.c_str());
            g_targetBacklog.erase(g_targetBacklog.begin() + i);
        } else {
            ++i;
        }
    }

    if (g_targetBacklog.empty()) {
        return;
    }
    PendingLine line = g_targetBacklog.front();
    if (!TickReached(now, line.activationTick) || !CanStartLineLocked(now, line.priority)) {
        return;
    }
    if (g_active.active) {
        if (line.priority >= 97 && line.priority >= g_active.priority) {
            ClearActiveLocked(now, "target-critical-replace");
        } else {
            return;
        }
    }
    if (g_pending.pending) {
        if (line.priority >= 97 && line.priority >= g_pending.priority) {
            CommentaryLog("drop-pending seq=%llu category=%s reason=target-critical-replace text=%s\n",
                          g_pending.eventSequence,
                          g_pending.category.c_str(),
                          g_pending.text.c_str());
            g_pending = PendingLine();
        } else {
            return;
        }
    }
    g_targetBacklog.erase(g_targetBacklog.begin());
    ActivatePendingLocked(line, now, activeCameraTarget, shotKind);
}

bool ShouldEmitNormalKill(const DirectorEvent& event, DWORD now) {
    if (g_lastGlobalKillLine != 0 &&
        static_cast<int>(now - g_lastGlobalKillLine) < static_cast<int>(kGlobalKillCooldownMs)) {
        return false;
    }
    auto it = g_lastPlayerKillLine.find(event.killerHandle);
    if (it != g_lastPlayerKillLine.end() &&
        static_cast<int>(now - it->second) < static_cast<int>(kPlayerKillCooldownMs)) {
        return false;
    }
    return true;
}

unsigned int UpdateRapidKillCount(const DirectorEvent& event, const ServerTelemetryKillEvent* telemetry) {
    bool suicide = telemetry && (telemetry->flags & STREAMER_KILL_FLAG_SUICIDE) != 0;
    bool teamkill = telemetry && (telemetry->flags & STREAMER_KILL_FLAG_TEAMKILL) != 0;
    if (suicide || teamkill) {
        return 0;
    }

    LocalPlayerState& victimState = g_playerStates[event.victimHandle];
    victimState.rapidKills = 0;
    victimState.lastRapidKillMs = 0;

    LocalPlayerState& killerState = g_playerStates[event.killerHandle];
    if (killerState.lastRapidKillMs != 0 &&
        static_cast<int>(event.rawTick - killerState.lastRapidKillMs) <= static_cast<int>(kRapidComboWindowMs)) {
        ++killerState.rapidKills;
    } else {
        killerState.rapidKills = 1;
    }
    killerState.lastRapidKillMs = event.rawTick;
    return static_cast<unsigned int>(killerState.rapidKills);
}

void RefreshTelemetryCachesLocked(DWORD now) {
    if (g_lastTelemetryCacheTick != 0
        && static_cast<int>(now - g_lastTelemetryCacheTick) < 1000) {
        return;
    }
    g_lastTelemetryCacheTick = now;

    std::vector<ServerTelemetryStatsDelta> stats;
    ServerTelemetry_CopyStats(stats);
    for (const ServerTelemetryStatsDelta& stat : stats) {
        if (stat.playerId <= 0) continue;
        auto it = g_latestStats.find(stat.playerId);
        if (it == g_latestStats.end() || stat.sequence >= it->second.sequence) {
            g_latestStats[stat.playerId] = stat;
        }
    }

    std::vector<ServerTelemetryKillEvent> kills;
    ServerTelemetry_CopyKills(kills);
    for (const ServerTelemetryKillEvent& kill : kills) {
        if (kill.sequence == 0) continue;
        if (!g_seenKillSequences.insert(kill.sequence).second) continue;
        bool suicide = (kill.flags & STREAMER_KILL_FLAG_SUICIDE) != 0;
        if (!suicide && kill.killerId > 0) {
            ++g_localKills[kill.killerId];
        }
        if (kill.victimId > 0) {
            ++g_localDeaths[kill.victimId];
        }
    }
}

int MetricRank(unsigned int value, unsigned int ServerTelemetryStatsDelta::*member) {
    if (value == 0) return 0;
    int rank = 1;
    for (const auto& item : g_latestStats) {
        if (item.second.*member > value) {
            ++rank;
        }
    }
    return rank;
}

int MetricTeamRank(int handle, int team, unsigned int value, unsigned int ServerTelemetryStatsDelta::*member, int* outCount) {
    if (outCount) *outCount = 0;
    if (team != 0 && team != 1) return 0;
    if (value == 0) return 0;

    int rank = 1;
    int count = 0;
    for (const auto& item : g_latestStats) {
        const ServerTelemetryStatsDelta& other = item.second;
        int otherTeam = (other.team == 0 || other.team == 1) ? other.team : IdentityTeamForHandle(other.playerId);
        if (otherTeam != team) continue;
        if (other.*member == 0 && item.first != handle) continue;
        ++count;
        if (other.*member > value) {
            ++rank;
        }
    }
    if (outCount) *outCount = count;
    return count > 0 ? rank : 0;
}

int MetricTeamWorstRank(int handle, int team, unsigned int value, unsigned int ServerTelemetryStatsDelta::*member, int* outCount) {
    if (outCount) *outCount = 0;
    if (team != 0 && team != 1) return 0;

    int rank = 1;
    int count = 0;
    for (const auto& item : g_latestStats) {
        const ServerTelemetryStatsDelta& other = item.second;
        int otherTeam = (other.team == 0 || other.team == 1) ? other.team : IdentityTeamForHandle(other.playerId);
        if (otherTeam != team) continue;
        if (other.*member == 0 && item.first != handle) continue;
        ++count;
        if (other.*member < value) {
            ++rank;
        }
    }
    if (outCount) *outCount = count;
    return count > 0 ? rank : 0;
}

const char* RankingPhrase(int rank, bool worst) {
    if (rank <= 0) return "unranked";
    if (rank == 1) return worst ? "worst" : "best";
    if (rank == 2) return worst ? "second worst" : "second best";
    if (rank == 3) return worst ? "third worst" : "third best";
    return worst ? "painfully average" : "respectable";
}

const char* RankPhrase(int rank) {
    if (rank == 1) return "Best";
    if (rank == 2) return "Second best";
    if (rank == 3) return "Third best";
    return "Respectable";
}

unsigned int AccuracyPercent(const ServerTelemetryStatsDelta& stat) {
    if (stat.accuracyShots == 0) return 0;
    return static_cast<unsigned int>(
        (static_cast<unsigned long long>(stat.accuracyHits) * 100ull + stat.accuracyShots / 2)
        / stat.accuracyShots);
}

const char* OrdinalSuffix(unsigned int value) {
    unsigned int mod100 = value % 100;
    if (mod100 >= 11 && mod100 <= 13) return "th";
    switch (value % 10) {
        case 1: return "st";
        case 2: return "nd";
        case 3: return "rd";
        default: return "th";
    }
}

const char* PossessiveSuffix(const char* name) {
    if (!name || !name[0]) return "'s";
    size_t len = strlen(name);
    char last = name[len - 1];
    return (last == 's' || last == 'S') ? "'" : "'s";
}

bool AppendText(char* buffer, size_t bufferLen, const char* suffix) {
    if (!buffer || bufferLen == 0 || !suffix || !suffix[0]) return false;
    size_t len = strlen(buffer);
    if (len + 1 >= bufferLen) return false;
    strncat_s(buffer, bufferLen, suffix, _TRUNCATE);
    return true;
}

bool BuildInlineKillStatSuffix(int handle,
                               const ResolvedName& name,
                               unsigned long long sequence,
                               char* out,
                               size_t outLen) {
    if (!out || outLen == 0 || handle <= 0) return false;
    out[0] = '\0';

    std::vector<int> choices;
    unsigned int kills = g_localKills[handle];
    unsigned int deaths = g_localDeaths[handle];
    auto statIt = g_latestStats.find(handle);
    if (kills >= 2) choices.push_back(0);
    if (statIt != g_latestStats.end() && statIt->second.accuracyShots >= 10) choices.push_back(1);
    if (statIt != g_latestStats.end() && statIt->second.totalDamage >= 300) choices.push_back(2);
    if (kills + deaths >= 4) choices.push_back(3);
    if (statIt != g_latestStats.end() && statIt->second.distanceMeters >= 500) choices.push_back(4);
    if (choices.empty()) return false;

    int choice = choices[(sequence + static_cast<unsigned long long>(handle)) % choices.size()];
    const ServerTelemetryStatsDelta* stat = statIt != g_latestStats.end() ? &statIt->second : nullptr;
    switch (choice) {
        case 0:
            sprintf_s(out,
                      outLen,
                      " That's his %u%s kill.",
                      kills,
                      OrdinalSuffix(kills));
            break;
        case 1:
            sprintf_s(out,
                      outLen,
                      " %s's accuracy is %u%% this round.",
                      name.name,
                      AccuracyPercent(*stat));
            break;
        case 2:
            sprintf_s(out,
                      outLen,
                      " %u damage this round. Subtle as a brick.",
                      stat->totalDamage);
            break;
        case 3: {
            float kd = deaths == 0 ? static_cast<float>(kills) : static_cast<float>(kills) / static_cast<float>(deaths);
            sprintf_s(out,
                      outLen,
                      " %.1f KD right now.",
                      kd);
            break;
        }
        case 4:
            sprintf_s(out,
                      outLen,
                      " %u m run already. Apparently legs are enabled.",
                      stat->distanceMeters);
            break;
        default:
            return false;
    }

    return out[0] != '\0';
}

bool ShouldAppendInlineStat(unsigned long long sequence, DWORD now, int handle) {
    return ((sequence + static_cast<unsigned long long>(now / 1000) + static_cast<unsigned long long>(handle)) % 2) == 0;
}

bool BuildTeamRankStatLine(int handle,
                           int team,
                           const ResolvedName& name,
                           const ServerTelemetryStatsDelta& stat,
                           unsigned int kills,
                           unsigned int deaths,
                           unsigned int cursor,
                           char* buffer,
                           size_t bufferLen) {
    if (!buffer || bufferLen == 0 || team != 0 && team != 1) return false;

    struct RankChoice {
        int id;
        bool worst;
    };
    std::vector<RankChoice> choices;

    int count = 0;
    int damageRank = MetricTeamRank(handle, team, stat.totalDamage, &ServerTelemetryStatsDelta::totalDamage, &count);
    if (count >= 3 && stat.totalDamage >= 100 && damageRank > 0) {
        choices.push_back({0, false});
    }

    int distanceRank = MetricTeamRank(handle, team, stat.distanceMeters, &ServerTelemetryStatsDelta::distanceMeters, &count);
    int distanceWorstRank = MetricTeamWorstRank(handle, team, stat.distanceMeters, &ServerTelemetryStatsDelta::distanceMeters, &count);
    if (count >= 3 && stat.distanceMeters >= 150 && distanceRank > 0) {
        choices.push_back({1, false});
    }
    if (count >= 3 && distanceWorstRank > 0 && distanceWorstRank <= 2) {
        choices.push_back({2, true});
    }

    unsigned int accuracy = AccuracyPercent(stat);
    int accuracyRank = 0;
    int accuracyWorstRank = 0;
    if (stat.accuracyShots >= 10) {
        int better = 0;
        int worse = 0;
        count = 0;
        for (const auto& item : g_latestStats) {
            const ServerTelemetryStatsDelta& other = item.second;
            int otherTeam = (other.team == 0 || other.team == 1) ? other.team : IdentityTeamForHandle(other.playerId);
            if (otherTeam != team || other.accuracyShots < 10) continue;
            ++count;
            unsigned int otherAccuracy = AccuracyPercent(other);
            if (otherAccuracy > accuracy) ++better;
            if (otherAccuracy < accuracy) ++worse;
        }
        accuracyRank = better + 1;
        accuracyWorstRank = worse + 1;
        if (count >= 3) {
            choices.push_back({3, false});
            if (accuracyWorstRank <= 2) {
                choices.push_back({4, true});
            }
        }
    }

    float kd = deaths == 0 ? static_cast<float>(kills) : static_cast<float>(kills) / static_cast<float>(deaths);
    if (kills + deaths >= 3) {
        choices.push_back({5, kd < 0.75f});
    }

    if (choices.empty()) return false;

    RankChoice choice = choices[cursor % choices.size()];
    switch (choice.id) {
        case 0:
            sprintf_s(buffer,
                      bufferLen,
                      "We are watching %s. %s is the %s damage dealer on the %s team with %u damage. Do with that information what you will.",
                      name.name,
                      name.name,
                      RankingPhrase(damageRank, false),
                      TeamName(team),
                      stat.totalDamage);
            break;
        case 1:
            sprintf_s(buffer,
                      bufferLen,
                      "We are watching %s. %s has the %s legs on the %s team at %u m. Cardio propaganda is working.",
                      name.name,
                      name.name,
                      RankingPhrase(distanceRank, false),
                      TeamName(team),
                      stat.distanceMeters);
            break;
        case 2:
            sprintf_s(buffer,
                      bufferLen,
                      "We are watching %s. %s is the %s mover on the %s team. Tactical standing around, apparently.",
                      name.name,
                      name.name,
                      RankingPhrase(distanceWorstRank, true),
                      TeamName(team));
            break;
        case 3:
            sprintf_s(buffer,
                      bufferLen,
                      "We are watching %s. %s has the %s accuracy on the %s team at %u%%. Annoying, but measurable.",
                      name.name,
                      name.name,
                      RankingPhrase(accuracyRank, false),
                      TeamName(team),
                      accuracy);
            break;
        case 4:
            sprintf_s(buffer,
                      bufferLen,
                      "We are watching %s. %s has the %s accuracy on the %s team at %u%%. The walls are terrified.",
                      name.name,
                      name.name,
                      RankingPhrase(accuracyWorstRank, true),
                      TeamName(team),
                      accuracy);
            break;
        case 5:
            if (choice.worst) {
                sprintf_s(buffer,
                          bufferLen,
                          "We are watching %s. %.1f KD on the %s team. This is less a performance, more a cry for help.",
                          name.name,
                          kd,
                          TeamName(team));
            } else {
                sprintf_s(buffer,
                          bufferLen,
                          "We are watching %s. %.1f KD for the %s team. Useful, which is rude to everyone else.",
                          name.name,
                          kd,
                          TeamName(team));
            }
            break;
        default:
            return false;
    }

    return buffer[0] != '\0';
}

bool BuildStatLine(int handle, std::string& outText, const char** outNameSource) {
    auto it = g_latestStats.find(handle);
    if (it == g_latestStats.end()) return false;

    const ServerTelemetryStatsDelta& stat = it->second;
    int team = (stat.team == 0 || stat.team == 1) ? stat.team : IdentityTeamForHandle(handle);
    ResolvedName name = ResolveNameWithRoleFallback(handle,
                                                    stat.playerName,
                                                    "stats-telemetry",
                                                    team,
                                                    "player",
                                                    "watched player");
    if (!name.ok) return false;

    std::vector<int> choices;
    if (stat.distanceMeters >= 400) choices.push_back(0);
    if (stat.flagDistanceMeters >= 80) choices.push_back(1);
    if (stat.totalDamage >= 350) choices.push_back(2);
    if (stat.accuracyShots >= 10) choices.push_back(3);
    if (stat.teamkills > 0) choices.push_back(4);
    if (stat.aliveTimeSec >= 120) choices.push_back(5);
    if (stat.flagAttempts >= 2) choices.push_back(6);
    unsigned int kills = g_localKills[handle];
    unsigned int deaths = g_localDeaths[handle];
    if (kills + deaths >= 3) choices.push_back(7);
    if (choices.empty()) return false;

    unsigned int& cursor = g_statCursor[handle];
    int choice = choices[cursor % choices.size()];
    ++cursor;

    char buffer[260] = {};
    if ((cursor % 3) == 0
        && BuildTeamRankStatLine(handle,
                                 team,
                                 name,
                                 stat,
                                 kills,
                                 deaths,
                                 cursor / 3,
                                 buffer,
                                 sizeof(buffer))) {
        outText = buffer;
        if (outNameSource) *outNameSource = name.source;
        return !outText.empty();
    }

    buffer[0] = '\0';
    switch (choice) {
        case 0: {
            int rank = MetricRank(stat.distanceMeters, &ServerTelemetryStatsDelta::distanceMeters);
            sprintf_s(buffer,
                      "%s has run %u m this round. %s legs on the server.",
                      name.name,
                      stat.distanceMeters,
                      RankPhrase(rank));
            break;
        }
        case 1:
            sprintf_s(buffer,
                      "%s dragged the flag %u m. The cardio is doing the carrying.",
                      name.name,
                      stat.flagDistanceMeters);
            break;
        case 2:
            sprintf_s(buffer,
                      "%s has dealt %u damage. Subtle as a brick.",
                      name.name,
                      stat.totalDamage);
            break;
        case 3: {
            unsigned int pct = AccuracyPercent(stat);
            sprintf_s(buffer,
                      "%s has %u%% accuracy. Not bad, suspiciously competent.",
                      name.name,
                      pct);
            break;
        }
        case 4:
            sprintf_s(buffer,
                      "%s has %u teamkill%s. Put the safety back on.",
                      name.name,
                      stat.teamkills,
                      stat.teamkills == 1 ? "" : "s");
            break;
        case 5:
            sprintf_s(buffer,
                      "%s stayed alive %u min. Cowardice with results.",
                      name.name,
                      stat.aliveTimeSec / 60);
            break;
        case 6:
            sprintf_s(buffer,
                      "%s has %u flag attempts. Persistence, or no short-term memory.",
                      name.name,
                      stat.flagAttempts);
            break;
        case 7: {
            float kd = deaths == 0 ? static_cast<float>(kills) : static_cast<float>(kills) / static_cast<float>(deaths);
            if (kd < 0.5f) {
                sprintf_s(buffer,
                          "%s has a %.1f KD. Get your shit together, %s.",
                          name.name,
                          kd,
                          name.name);
            } else if (kd >= 2.0f) {
                sprintf_s(buffer,
                          "%s has a %.1f KD. Annoying, but effective.",
                          name.name,
                          kd);
            } else {
                sprintf_s(buffer,
                          "%s has a %.1f KD. Perfectly mid, professionally delivered.",
                          name.name,
                          kd);
            }
            break;
        }
        default:
            return false;
    }

    outText = buffer;
    if (outNameSource) *outNameSource = name.source;
    return !outText.empty();
}

bool BuildKillLine(const DirectorEvent& event,
                   DWORD now,
                   int activeCameraTarget,
                   const char* shotKind,
                   std::string& outText,
                   int& outPriority,
                   const char** outNameSource,
                   const char** outCategory) {
    if (event.victimHandle <= 0) return false;

    ServerTelemetryKillEvent kill = {};
    bool hasKill = ServerTelemetry_TryFindKill(event.killerHandle, event.victimHandle, event.rawTick, &kill);

    const bool suicide = hasKill && (kill.flags & STREAMER_KILL_FLAG_SUICIDE) != 0;
    const bool teamkill = hasKill && (kill.flags & STREAMER_KILL_FLAG_TEAMKILL) != 0;
    const bool headshot = hasKill && (kill.flags & STREAMER_KILL_FLAG_HEADSHOT) != 0;
    const char* weapon = hasKill && kill.weaponName[0] ? kill.weaponName : event.serverWeaponName;
    if (!weapon || !weapon[0]) weapon = nullptr;

    bool victimIsFlagCarrier = !suicide
        && (IsKnownFlagCarrier(event.victimHandle)
            || (activeCameraTarget == event.victimHandle && IsFlagShot(shotKind)));
    bool killerIsFlagCarrier = !suicide
        && event.killerHandle > 0
        && (IsKnownFlagCarrier(event.killerHandle)
            || (activeCameraTarget == event.killerHandle && IsFlagShot(shotKind)));
    bool watchedTargetInvolved = activeCameraTarget != 0
        && (event.killerHandle == activeCameraTarget
            || event.victimHandle == activeCameraTarget);
    int victimCarrierTeam = KnownCarrierTeam(event.victimHandle);
    int killerTeam = hasKill ? kill.killerTeam : IdentityTeamForHandle(event.killerHandle);
    int victimTeam = hasKill ? kill.victimTeam : victimCarrierTeam;

    ResolvedName killerName = ResolveNameWithRoleFallback(event.killerHandle,
                                                          hasKill ? kill.killerName : nullptr,
                                                          "kill-telemetry",
                                                          killerTeam,
                                                          "attacker",
                                                          event.killerHandle == activeCameraTarget ? "watched player" : "attacker");
    ResolvedName victimName = ResolveNameWithRoleFallback(event.victimHandle,
                                                          hasKill ? kill.victimName : nullptr,
                                                          "kill-telemetry",
                                                          victimIsFlagCarrier ? victimCarrierTeam : victimTeam,
                                                          victimIsFlagCarrier ? "carrier" : "defender",
                                                          event.victimHandle == activeCameraTarget ? "watched player" : (victimIsFlagCarrier ? "flag carrier" : "defender"));

    bool recentCommentedKillerDied = !suicide
        && g_lastCommentedKill.valid
        && event.victimHandle == g_lastCommentedKill.killerHandle
        && event.killerHandle != 0
        && event.sequence != g_lastCommentedKill.eventSequence
        && TickReached(now, g_lastCommentedKill.lineTick)
        && !TickReached(now, g_lastCommentedKill.lineTick + kPostKillDeathWindowMs);

    if (recentCommentedKillerDied && killerName.ok) {
        UpdateRapidKillCount(event, hasKill ? &kill : nullptr);
        char buffer[320] = {};
        unsigned int variant = static_cast<unsigned int>(
            (event.sequence + static_cast<unsigned long long>(event.killerHandle)) % 5);
        if (variant == 0) {
            sprintf_s(buffer, "And he's gone. %s kills him right back.", killerName.name);
        } else if (variant == 1) {
            sprintf_s(buffer, "And he just got killed by %s. Short victory lap.", killerName.name);
        } else if (variant == 2) {
            sprintf_s(buffer, "Nice kill, shame about immediately dying to %s.", killerName.name);
        } else if (variant == 3) {
            sprintf_s(buffer, "%s trades him out. Celebration cancelled.", killerName.name);
        } else {
            sprintf_s(buffer, "And there goes the hero. %s shuts that down.", killerName.name);
        }
        outText = buffer;
        outPriority = 96;
        if (outNameSource) *outNameSource = killerName.source;
        if (outCategory) *outCategory = "post-kill-death";
        g_lastCommentedKill.valid = false;
        return !outText.empty();
    }

    unsigned int rapidKills = UpdateRapidKillCount(event, hasKill ? &kill : nullptr);
    ServerTelemetryAchievementEvent achievement = {};
    bool hasAchievement = ServerTelemetry_TryFindAchievement(event.killerHandle, event.rawTick, &achievement);
    unsigned int achievementId = hasAchievement ? achievement.achievementId : 0;
    if (achievementId == 0 && rapidKills >= 2) {
        achievementId = rapidKills == 2 ? 1 : (rapidKills == 3 ? 2 : 0);
    }

    char buffer[320] = {};
    outPriority = 45;
    if (victimIsFlagCarrier && event.killerHandle > 0 && killerName.ok && victimName.ok) {
        sprintf_s(buffer, "%s kills the flag carrier %s", killerName.name, victimName.name);
        outPriority = 100;
        if (outNameSource) *outNameSource = killerName.source;
        if (outCategory) *outCategory = "flag-carrier-kill";
    } else if (achievementId == 1 || achievementId == 2) {
        ResolvedName comboName = ResolveNameWithRoleFallback(
            achievement.playerId ? achievement.playerId : event.killerHandle,
            hasAchievement ? achievement.playerName : (hasKill ? kill.killerName : nullptr),
            hasAchievement ? "achievement-telemetry" : "kill-telemetry",
            hasAchievement ? achievement.playerTeam : killerTeam,
            "attacker",
            "attacker");
        if (!comboName.ok) return false;
        sprintf_s(buffer,
                  "%s gets a %s kill",
                  comboName.name,
                  achievementId == 1 ? "double" : "triple");
        outPriority = 95;
        if (outNameSource) *outNameSource = comboName.source;
        if (outCategory) *outCategory = "combo-kill";
    } else if (suicide) {
        if (!victimName.ok) return false;
        sprintf_s(buffer, "%s killed themselves", victimName.name);
        outPriority = 80;
        if (outNameSource) *outNameSource = victimName.source;
        if (outCategory) *outCategory = "suicide";
    } else {
        if (event.killerHandle <= 0 || !killerName.ok || !victimName.ok) {
            CommentaryLog("suppress-detail seq=%llu kill missing=%s%s killer=%d victim=%d telemetry=%d killerTeam=%d victimTeam=%d\n",
                          event.sequence,
                          killerName.ok ? "" : "killer",
                          victimName.ok ? "" : " victim",
                          event.killerHandle,
                          event.victimHandle,
                          hasKill ? 1 : 0,
                          killerTeam,
                          victimTeam);
            return false;
        }
        if (teamkill) {
            sprintf_s(buffer, "%s teamkills %s", killerName.name, victimName.name);
            outPriority = watchedTargetInvolved ? 94 : 85;
            if (outCategory) *outCategory = "teamkill";
        } else if (headshot) {
            if (killerIsFlagCarrier) {
                sprintf_s(buffer,
                          "%s headshots %s while carrying the flag. Absolute Rambo nonsense.",
                          killerName.name,
                          victimName.name);
                outPriority = 95;
                if (outCategory) *outCategory = "flag-carrier-kill-dealt";
            } else {
                sprintf_s(buffer, "%s headshots %s", killerName.name, victimName.name);
                outPriority = watchedTargetInvolved ? 92 : 75;
                if (outCategory) *outCategory = "headshot";
            }
        } else {
            if (!watchedTargetInvolved && !ShouldEmitNormalKill(event, now)) {
                return false;
            }
            if (killerIsFlagCarrier && weapon) {
                sprintf_s(buffer,
                          "%s kills %s with %s while carrying the flag. Rambo with paperwork.",
                          killerName.name,
                          victimName.name,
                          weapon);
                if (outCategory) *outCategory = "flag-carrier-kill-dealt";
                outPriority = 95;
            } else if (killerIsFlagCarrier) {
                sprintf_s(buffer,
                          "%s kills %s while carrying the flag. Rambo called, he approves.",
                          killerName.name,
                          victimName.name);
                if (outCategory) *outCategory = "flag-carrier-kill-dealt";
                outPriority = 95;
            } else if (weapon) {
                sprintf_s(buffer, "%s kills %s with %s", killerName.name, victimName.name, weapon);
                if (outCategory) *outCategory = "normal-kill";
            } else {
                sprintf_s(buffer, "%s kills %s", killerName.name, victimName.name);
                if (outCategory) *outCategory = "normal-kill";
            }
            if (watchedTargetInvolved && !killerIsFlagCarrier) {
                outPriority = event.victimHandle == activeCameraTarget ? 96 : 91;
            }
        }
        if (!teamkill
            && ShouldAppendInlineStat(event.sequence, now, event.killerHandle)) {
            char suffix[128] = {};
            if (BuildInlineKillStatSuffix(event.killerHandle,
                                          killerName,
                                          event.sequence,
                                          suffix,
                                          sizeof(suffix))) {
                AppendText(buffer, sizeof(buffer), suffix);
            }
        }
        if (outNameSource) *outNameSource = killerName.source;
    }

    outText = buffer;
    return !outText.empty();
}

bool InferFlagActionForLine(const DirectorEvent& event,
                            unsigned int* outAction,
                            int* outPlayerHandle,
                            int* outPlayerTeam) {
    if (!outAction || !outPlayerHandle || !outPlayerTeam) return false;

    unsigned int action = event.flagAction;
    int playerHandle = event.flagPlayerHandle;
    int playerTeam = event.flagPlayerTeam;
    if (event.hasFlagTelemetry && action != 0) {
        *outAction = action;
        *outPlayerHandle = playerHandle;
        *outPlayerTeam = playerTeam;
        return playerHandle != 0 || action == 3;
    }

    int previousUS = event.previousUSCarrier != 0 ? event.previousUSCarrier : g_lastContext.usCarrier;
    int previousVC = event.previousVCCarrier != 0 ? event.previousVCCarrier : g_lastContext.vcCarrier;

    if (action == 0) {
        if (previousUS == 0 && event.usCarrier != 0) {
            action = 1;
            playerHandle = event.usCarrier;
            playerTeam = 0;
        } else if (previousVC == 0 && event.vcCarrier != 0) {
            action = 1;
            playerHandle = event.vcCarrier;
            playerTeam = 1;
        } else if (previousUS != 0 && event.usCarrier == 0) {
            action = 2;
            playerHandle = previousUS;
            playerTeam = 0;
        } else if (previousVC != 0 && event.vcCarrier == 0) {
            action = 2;
            playerHandle = previousVC;
            playerTeam = 1;
        }
    }

    if (playerHandle == 0 && action == 2) {
        if (g_lastContext.flagScoreHoldTarget != 0) {
            playerHandle = g_lastContext.flagScoreHoldTarget;
        } else if (previousUS != 0) {
            playerHandle = previousUS;
            playerTeam = 0;
        } else if (previousVC != 0) {
            playerHandle = previousVC;
            playerTeam = 1;
        }
    }

    if (playerTeam != 0 && playerTeam != 1) {
        if (playerHandle == event.usCarrier || playerHandle == previousUS) playerTeam = 0;
        else if (playerHandle == event.vcCarrier || playerHandle == previousVC) playerTeam = 1;
        else playerTeam = IdentityTeamForHandle(playerHandle);
    }

    *outAction = action;
    *outPlayerHandle = playerHandle;
    *outPlayerTeam = playerTeam;
    return action != 0 && playerHandle != 0;
}

bool BuildFlagLine(const DirectorEvent& event,
                   std::string& outText,
                   int& outPriority,
                   const char** outNameSource,
                   const char** outCategory) {
    unsigned int action = 0;
    int playerHandle = 0;
    int playerTeam = -1;
    if (!InferFlagActionForLine(event, &action, &playerHandle, &playerTeam)) {
        return false;
    }

    const char* role = action == 3 ? "defender" : "carrier";
    ResolvedName playerName = ResolveNameWithRoleFallback(playerHandle,
                                                          event.flagPlayerName,
                                                          event.hasFlagTelemetry ? "flag-telemetry" : "flag-local",
                                                          playerTeam,
                                                          role,
                                                          action == 3 ? "defender" : "flag carrier");
    if (!playerName.ok) {
        CommentaryLog("suppress-detail seq=%llu flag missing=player player=%d action=%u telemetry=%d team=%d\n",
                      event.sequence,
                      playerHandle,
                      action,
                      event.hasFlagTelemetry ? 1 : 0,
                      playerTeam);
        return false;
    }

    char buffer[220] = {};
    outPriority = 92;
    if (action == 1) {
        auto statIt = g_latestStats.find(playerHandle);
        unsigned int flagAttempts = statIt != g_latestStats.end() ? statIt->second.flagAttempts : 0;
        if (flagAttempts >= 2) {
            unsigned int variant = static_cast<unsigned int>(
                (event.sequence + static_cast<unsigned long long>(playerHandle)) % 3);
            if (variant == 0) {
                sprintf_s(buffer,
                          "This is %s%s %u%s flag attempt. We wish them luck, because skill may not be enough.",
                          playerName.name,
                          PossessiveSuffix(playerName.name),
                          flagAttempts,
                          OrdinalSuffix(flagAttempts));
            } else if (variant == 1) {
                sprintf_s(buffer,
                          "%s grabs the flag again. Attempt %u. Stubborn bastard.",
                          playerName.name,
                          flagAttempts);
            } else {
                sprintf_s(buffer,
                          "%s is on flag attempt number %u. Surely this one is not cursed.",
                          playerName.name,
                          flagAttempts);
            }
        } else if ((event.flagReasonFlags & STREAMER_FLAG_REASON_FROM_GROUND) != 0) {
            sprintf_s(buffer, "%s steals the flag", playerName.name);
        } else {
            sprintf_s(buffer, "%s is taking the enemy flag", playerName.name);
        }
        if (outCategory) *outCategory = "flag-take";
    } else if (action == 2 && (event.flagReasonFlags & STREAMER_FLAG_REASON_SCORE) != 0) {
        unsigned int meters = static_cast<unsigned int>(event.flagCarryDistanceMeters + 0.5f);
        if (meters > 0 && (playerTeam == 0 || playerTeam == 1)) {
            sprintf_s(buffer,
                      "%s scores for %s after a %u m flag run",
                      playerName.name,
                      TeamName(playerTeam),
                      meters);
        } else if (playerTeam == 0 || playerTeam == 1) {
            sprintf_s(buffer, "%s scores for %s", playerName.name, TeamName(playerTeam));
        } else {
            sprintf_s(buffer, "%s scores with the flag", playerName.name);
        }
        outPriority = 100;
        if (outCategory) *outCategory = "flag-score";
    } else if (action == 2) {
        sprintf_s(buffer, "%s drops the enemy flag", playerName.name);
        outPriority = 94;
        if (outCategory) *outCategory = "flag-drop";
    } else if (action == 3) {
        if (event.flagSide <= 1) {
            sprintf_s(buffer,
                      "%s returns the %s flag",
                      playerName.name,
                      TeamName(static_cast<int>(event.flagSide)));
        } else {
            sprintf_s(buffer, "%s returns the flag", playerName.name);
        }
        outPriority = 94;
        if (outCategory) *outCategory = "flag-return";
    } else {
        return false;
    }

    outText = buffer;
    if (outNameSource) *outNameSource = playerName.source;
    return !outText.empty();
}

DWORD NextFlagLoopDelay(DWORD now, int handle) {
    return 10000 + ((now / 97 + static_cast<DWORD>(handle * 131)) % 10000);
}

bool BuildFlagLoopLine(const CommentaryCameraContext& context,
                       std::string& outText,
                       const char** outNameSource) {
    int handle = context.activeCameraTarget;
    if (handle == 0) return false;

    int team = -1;
    bool moving = false;
    bool camping = false;
    if (handle == context.usCarrier) {
        team = 0;
        moving = context.usCarrierMoving;
        camping = context.usCarrierCamping;
    } else if (handle == context.vcCarrier) {
        team = 1;
        moving = context.vcCarrierMoving;
        camping = context.vcCarrierCamping;
    } else {
        return false;
    }

    if (!IsFlagShot(context.shotKind)) return false;

    ResolvedName name = ResolveNameWithRoleFallback(handle,
                                                    nullptr,
                                                    nullptr,
                                                    team,
                                                    "carrier",
                                                    "watched player");
    if (!name.ok) return false;

    DWORD nextTick = g_nextFlagLoopTick[handle];
    DWORD now = context.currentTick;
    if (nextTick == 0) {
        g_nextFlagLoopTick[handle] = now + 5000 + ((now / 131 + static_cast<DWORD>(handle * 47)) % 4000);
        return false;
    }
    if (!TickReached(now, nextTick)) return false;
    if (!moving && !camping) {
        g_nextFlagLoopTick[handle] = now + NextFlagLoopDelay(now, handle);
        return false;
    }

    char buffer[220] = {};
    unsigned int variant = (now / 1000 + handle) % 4;
    if (camping) {
        if (variant % 2 == 0) {
            sprintf_s(buffer, "%s is camping with the flag. Bold choice, if boredom is the plan.", name.name);
        } else {
            sprintf_s(buffer, "%s has the flag and has stopped moving. Terrific, a hostage situation.", name.name);
        }
    } else if (moving) {
        if (variant == 0) {
            sprintf_s(buffer, "%s is running with the flag", name.name);
        } else if (variant == 1) {
            sprintf_s(buffer, "%s is running with the flag like cardio pays rent.", name.name);
        } else if (variant == 2) {
            sprintf_s(buffer, "%s still has the flag. The defense may want to wake up.", name.name);
        } else {
            sprintf_s(buffer, "%s is dragging the flag across the map. Subtle work.", name.name);
        }
    } else {
        sprintf_s(buffer, "%s has the flag. Move it or frame it, your call.", name.name);
    }

    outText = buffer;
    if (outNameSource) *outNameSource = name.source;
    g_nextFlagLoopTick[handle] = now + NextFlagLoopDelay(now, handle);
    return !outText.empty();
}

const char* OtherTeamName(int team) {
    return TeamName(team == 0 ? 1 : 0);
}

int LeadingTeam(int us, int vc) {
    if (us > vc) return 0;
    if (vc > us) return 1;
    return -1;
}

bool SelectGeneralTemplate(const char* const* templates,
                           size_t templateCount,
                           const char* category,
                           DWORD seed,
                           bool allowCategoryRepeat,
                           const char** outTemplate,
                           std::string& outTemplateKey) {
    if (!templates || templateCount == 0 || !outTemplate) return false;
    size_t start = static_cast<size_t>(seed % static_cast<DWORD>(templateCount));
    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t offset = 0; offset < templateCount; ++offset) {
            size_t index = (start + offset) % templateCount;
            char key[64] = {};
            sprintf_s(key, "%s:%u", category, static_cast<unsigned int>(index));
            if (pass == 0 && !CanUseGeneralTemplate(key)) continue;
            *outTemplate = templates[index];
            outTemplateKey = key;
            return true;
        }
    }
    return false;
}

bool CategoryAllowedForGeneral(const char* category, bool allowCategoryRepeat) {
    if (allowCategoryRepeat) return true;
    return g_lastGeneralCategory.empty() || g_lastGeneralCategory != category;
}

bool BuildScoreChangeLineLocked(DWORD now, GeneralCommentCandidate& out) {
    if (!g_pendingScoreChange.pending) return false;
    if (!TickReached(now, g_pendingScoreChange.eligibleTick)) return false;
    if (TickReached(now, g_pendingScoreChange.expireTick)) {
        g_pendingScoreChange = PendingScoreChange();
        return false;
    }
    if (g_lastScoreCommentTick != 0
        && !TickReached(now, g_lastScoreCommentTick + kScoreCommentMinIntervalMs)) {
        g_pendingScoreChange = PendingScoreChange();
        return false;
    }

    int oldUs = g_pendingScoreChange.oldUs;
    int oldVc = g_pendingScoreChange.oldVc;
    int newUs = g_pendingScoreChange.newUs;
    int newVc = g_pendingScoreChange.newVc;
    int deltaUs = newUs - oldUs;
    int deltaVc = newVc - oldVc;
    int scoringTeam = deltaVc > deltaUs ? 1 : 0;
    int oldLeader = LeadingTeam(oldUs, oldVc);
    int newLeader = LeadingTeam(newUs, newVc);

    const char* category = "game-score-change";
    static const char* kScoreChanged[] = {
        "%s score. It is %d-%d now; %s are making this harder than it needs to be.",
        "%s put points on the board. %d-%d, and %s need to stop donating.",
        "%s cash in. Score is %d-%d; %s just got taxed."
    };
    static const char* kLeadChanged[] = {
        "%s take the lead %d-%d. %s just got mugged by the scoreboard.",
        "%s jump ahead %d-%d. %s are officially chasing the mess now.",
        "%s steal the lead %d-%d. %s blinked first."
    };
    static const char* kTieBroken[] = {
        "%s break the tie, %d-%d. %s blinked first.",
        "%s crack the tie open %d-%d. %s are back in the swamp.",
        "%s untie it, %d-%d. %s just handed over momentum."
    };
    static const char* kLeadWide[] = {
        "%s stretch it to %d-%d. %s are getting dragged into paperwork.",
        "%s widen the gap, %d-%d. %s need answers, preferably loaded ones.",
        "%s push the lead to %d-%d. %s are eating the scoreboard raw."
    };
    static const char* kLeadNarrow[] = {
        "%s narrow it to %d-%d. %s still lead, but the chair is wobbling.",
        "%s cut the gap to %d-%d. %s are not comfortable, no matter what they say.",
        "%s claw one back, %d-%d. %s still lead, barely acting like it."
    };

    const char* const* pool = kScoreChanged;
    size_t poolCount = sizeof(kScoreChanged) / sizeof(kScoreChanged[0]);
    if (oldLeader == -1 && newLeader != -1) {
        category = "game-tie-break";
        pool = kTieBroken;
        poolCount = sizeof(kTieBroken) / sizeof(kTieBroken[0]);
    } else if (oldLeader != newLeader && newLeader != -1) {
        category = "game-lead-change";
        pool = kLeadChanged;
        poolCount = sizeof(kLeadChanged) / sizeof(kLeadChanged[0]);
    } else if (newLeader == scoringTeam && abs(newUs - newVc) > abs(oldUs - oldVc)) {
        category = "game-lead-wide";
        pool = kLeadWide;
        poolCount = sizeof(kLeadWide) / sizeof(kLeadWide[0]);
    } else if (newLeader != -1 && newLeader != scoringTeam && abs(newUs - newVc) < abs(oldUs - oldVc)) {
        category = "game-lead-narrow";
        pool = kLeadNarrow;
        poolCount = sizeof(kLeadNarrow) / sizeof(kLeadNarrow[0]);
    }

    const char* tmpl = nullptr;
    std::string templateKey;
    if (!SelectGeneralTemplate(pool,
                               poolCount,
                               category,
                               now + static_cast<DWORD>(newUs * 31 + newVc * 17),
                               true,
                               &tmpl,
                               templateKey)) {
        return false;
    }

    char buffer[260] = {};
    sprintf_s(buffer,
              tmpl,
              TeamName(scoringTeam),
              newUs,
              newVc,
              OtherTeamName(scoringTeam));
    out.category = category;
    out.templateKey = templateKey;
    out.text = buffer;
    g_pendingScoreChange = PendingScoreChange();
    g_lastScoreCommentTick = now;
    return !out.text.empty();
}

bool BuildStableScoreLineLocked(DWORD now, GeneralCommentCandidate& out) {
    if (!g_teamScore.valid) return false;
    if (g_nextStableScoreTick == 0) {
        g_nextStableScoreTick = now + RandomDelay(now, kStableScoreMinIntervalMs, kStableScoreMaxIntervalMs);
        return false;
    }
    if (!TickReached(now, g_nextStableScoreTick)) return false;

    const char* category = "game-stable-score";
    if (!CategoryAllowedForGeneral(category, false)) {
        g_nextStableScoreTick = now + RandomDelay(now + 19, 45000, 90000);
        return false;
    }

    static const char* kStable[] = {
        "Still %d-%d. Both teams are apparently filing taxes between firefights.",
        "No score movement: %d-%d. Tactical constipation from everyone involved.",
        "Score is stuck at %d-%d. Somebody do something useful.",
        "%d-%d and frozen. The scoreboard has more discipline than the teams."
    };

    const char* tmpl = nullptr;
    std::string templateKey;
    if (!SelectGeneralTemplate(kStable,
                               sizeof(kStable) / sizeof(kStable[0]),
                               category,
                               now,
                               false,
                               &tmpl,
                               templateKey)) {
        return false;
    }

    char buffer[240] = {};
    sprintf_s(buffer, tmpl, g_teamScore.us, g_teamScore.vc);
    out.category = category;
    out.templateKey = templateKey;
    out.text = buffer;
    g_nextStableScoreTick = now + RandomDelay(now + 23, kStableScoreMinIntervalMs, kStableScoreMaxIntervalMs);
    return !out.text.empty();
}

TeamAggregateStats BuildTeamAggregate(int team) {
    TeamAggregateStats aggregate = {};
    for (const auto& item : g_latestStats) {
        const ServerTelemetryStatsDelta& stat = item.second;
        int statTeam = (stat.team == 0 || stat.team == 1) ? stat.team : IdentityTeamForHandle(stat.playerId);
        if (statTeam != team) continue;
        ++aggregate.players;
        aggregate.totalDamage += stat.totalDamage;
        aggregate.distanceMeters += stat.distanceMeters;
        aggregate.flagDistanceMeters += stat.flagDistanceMeters;
        aggregate.flagAttempts += stat.flagAttempts;
        aggregate.accuracyShots += stat.accuracyShots;
        aggregate.accuracyHits += stat.accuracyHits;
        aggregate.teamkills += stat.teamkills;
        aggregate.aliveTimeSec += stat.aliveTimeSec;
    }
    return aggregate;
}

double AccuracyForAggregate(const TeamAggregateStats& stats) {
    if (stats.accuracyShots == 0) return 0.0;
    return static_cast<double>(stats.accuracyHits) * 100.0 / static_cast<double>(stats.accuracyShots);
}

void AddAggregateCandidate(std::vector<GeneralCommentCandidate>& candidates,
                           const char* category,
                           const char* const* templates,
                           size_t templateCount,
                           DWORD seed,
                           const char* leadingTeam,
                           const char* trailingTeam,
                           double firstValue,
                           bool wholeNumberValues) {
    if (!CategoryAllowedForGeneral(category, false)) return;
    const char* tmpl = nullptr;
    std::string templateKey;
    if (!SelectGeneralTemplate(templates, templateCount, category, seed, false, &tmpl, templateKey)) {
        return;
    }

    char buffer[280] = {};
    if (wholeNumberValues) {
        sprintf_s(buffer,
                  tmpl,
                  leadingTeam,
                  static_cast<unsigned int>(firstValue + 0.5),
                  trailingTeam);
    } else {
        sprintf_s(buffer, tmpl, leadingTeam, firstValue, trailingTeam);
    }
    GeneralCommentCandidate candidate;
    candidate.category = category;
    candidate.templateKey = templateKey;
    candidate.text = buffer;
    candidates.push_back(candidate);
}

void AddTeamkillAggregateCandidate(std::vector<GeneralCommentCandidate>& candidates,
                                   const char* const* templates,
                                   size_t templateCount,
                                   DWORD seed,
                                   const char* leadingTeam,
                                   const char* trailingTeam,
                                   unsigned int highTeamkills,
                                   unsigned int lowTeamkills) {
    const char* category = "game-teamkill-shame";
    if (!CategoryAllowedForGeneral(category, false)) return;
    const char* tmpl = nullptr;
    std::string templateKey;
    if (!SelectGeneralTemplate(templates, templateCount, category, seed, false, &tmpl, templateKey)) {
        return;
    }

    char buffer[280] = {};
    sprintf_s(buffer, tmpl, leadingTeam, highTeamkills, lowTeamkills, trailingTeam);
    GeneralCommentCandidate candidate;
    candidate.category = category;
    candidate.templateKey = templateKey;
    candidate.text = buffer;
    candidates.push_back(candidate);
}

bool BuildAggregateStatLineLocked(DWORD now, GeneralCommentCandidate& out) {
    if (g_nextAggregateStatTick == 0) {
        g_nextAggregateStatTick = now + RandomDelay(now + 41, kAggregateStatMinIntervalMs, kAggregateStatMaxIntervalMs);
        return false;
    }
    if (!TickReached(now, g_nextAggregateStatTick)) return false;
    g_nextAggregateStatTick = now + RandomDelay(now + 43, kAggregateStatMinIntervalMs, kAggregateStatMaxIntervalMs);

    TeamAggregateStats us = BuildTeamAggregate(0);
    TeamAggregateStats vc = BuildTeamAggregate(1);
    if (us.players == 0 || vc.players == 0) return false;

    std::vector<GeneralCommentCandidate> candidates;
    static const char* kDamage[] = {
        "%s have dealt %u more damage. %s are absorbing bullets like unpaid interns.",
        "%s are up %u damage. %s brought bodies, not answers.",
        "%s lead damage by %u. %s are getting turned into a health-bar charity."
    };
    static const char* kMovement[] = {
        "%s have run %.1f km more than %s. Great, a marathon with rifles.",
        "%s are out-running by %.1f km over %s. At least somebody packed legs.",
        "%s lead movement by %.1f km over %s. Cardio is apparently a strategy."
    };
    static const char* kAccuracy[] = {
        "%s are shooting %.0f points cleaner than %s. Try aiming, clowns.",
        "%s lead accuracy by %.0f points over %s. The other side is painting trees.",
        "%s are %.0f points sharper than %s. That is less spray, more problem."
    };
    static const char* kFlag[] = {
        "%s have %u more flag attempts. %s are guarding the objective like it owes them money.",
        "%s lead flag pressure by %u attempts. %s may want to locate the objective.",
        "%s are up %u flag attempts. %s are defending with vibes and prayers."
    };
    static const char* kTeamkill[] = {
        "%s lead teamkills %u-%u. Friendly fire, hostile intelligence.",
        "%s have the teamkill edge %u-%u. %s should fear their own payroll.",
        "%s are winning teamkills %u-%u. The enemy appreciates the outsourcing."
    };

    unsigned long long damageHigh = us.totalDamage >= vc.totalDamage ? us.totalDamage : vc.totalDamage;
    unsigned long long damageLow = us.totalDamage >= vc.totalDamage ? vc.totalDamage : us.totalDamage;
    if (damageHigh >= damageLow + 500
        || (damageLow > 0 && damageHigh * 100 >= damageLow * 120)) {
        int team = us.totalDamage >= vc.totalDamage ? 0 : 1;
        AddAggregateCandidate(candidates,
                              "game-damage",
                              kDamage,
                              sizeof(kDamage) / sizeof(kDamage[0]),
                              now + 101,
                              TeamName(team),
                              OtherTeamName(team),
                              static_cast<double>(damageHigh - damageLow),
                              true);
    }

    unsigned long long distanceHigh = us.distanceMeters >= vc.distanceMeters ? us.distanceMeters : vc.distanceMeters;
    unsigned long long distanceLow = us.distanceMeters >= vc.distanceMeters ? vc.distanceMeters : us.distanceMeters;
    if (distanceHigh >= distanceLow + 500
        || (distanceLow > 0 && distanceHigh * 100 >= distanceLow * 115)) {
        int team = us.distanceMeters >= vc.distanceMeters ? 0 : 1;
        AddAggregateCandidate(candidates,
                              "game-movement",
                              kMovement,
                              sizeof(kMovement) / sizeof(kMovement[0]),
                              now + 103,
                              TeamName(team),
                              OtherTeamName(team),
                              static_cast<double>(distanceHigh - distanceLow) / 1000.0,
                              false);
    }

    double usAccuracy = AccuracyForAggregate(us);
    double vcAccuracy = AccuracyForAggregate(vc);
    double accuracyDiff = usAccuracy > vcAccuracy ? usAccuracy - vcAccuracy : vcAccuracy - usAccuracy;
    if (accuracyDiff >= 5.0 && us.accuracyShots >= 25 && vc.accuracyShots >= 25) {
        int team = usAccuracy >= vcAccuracy ? 0 : 1;
        AddAggregateCandidate(candidates,
                              "game-accuracy",
                              kAccuracy,
                              sizeof(kAccuracy) / sizeof(kAccuracy[0]),
                              now + 107,
                              TeamName(team),
                              OtherTeamName(team),
                              accuracyDiff,
                              false);
    }

    unsigned long long flagHigh = us.flagAttempts >= vc.flagAttempts ? us.flagAttempts : vc.flagAttempts;
    unsigned long long flagLow = us.flagAttempts >= vc.flagAttempts ? vc.flagAttempts : us.flagAttempts;
    if (flagHigh >= flagLow + 2) {
        int team = us.flagAttempts >= vc.flagAttempts ? 0 : 1;
        AddAggregateCandidate(candidates,
                              "game-flag-pressure",
                              kFlag,
                              sizeof(kFlag) / sizeof(kFlag[0]),
                              now + 109,
                              TeamName(team),
                              OtherTeamName(team),
                              static_cast<double>(flagHigh - flagLow),
                              true);
    }

    if (us.teamkills != vc.teamkills && (us.teamkills > 0 || vc.teamkills > 0)) {
        int team = us.teamkills >= vc.teamkills ? 0 : 1;
        AddTeamkillAggregateCandidate(candidates,
                                      kTeamkill,
                                      sizeof(kTeamkill) / sizeof(kTeamkill[0]),
                                      now + 113,
                                      TeamName(team),
                                      OtherTeamName(team),
                                      static_cast<unsigned int>(team == 0 ? us.teamkills : vc.teamkills),
                                      static_cast<unsigned int>(team == 0 ? vc.teamkills : us.teamkills));
    }

    if (candidates.empty()) return false;
    size_t index = static_cast<size_t>((now / 1000) % candidates.size());
    out = candidates[index];
    return !out.text.empty();
}

bool TryQueueGameStateLineLocked(const CommentaryCameraContext& context) {
    DWORD now = context.currentTick;
    if (HasWatchedPlayerEventBacklogLocked()) return false;

    GeneralCommentCandidate candidate;
    if (!BuildScoreChangeLineLocked(now, candidate)
        && !BuildAggregateStatLineLocked(now, candidate)
        && !BuildStableScoreLineLocked(now, candidate)) {
        return false;
    }
    if (candidate.text.empty()) return false;

    PendingLine line = {};
    line.pending = true;
    line.activationTick = now;
    line.expireTick = now + 1500;
    line.requiredTarget = 0;
    line.priority = candidate.category.find("score") != std::string::npos ? 34 : 28;
    line.eventSequence = g_generalSequence++;
    line.category = candidate.category;
    line.nameSource = "game-state";
    line.templateKey = candidate.templateKey;
    line.text = candidate.text;
    SetPendingLocked(line, now, "game-state");
    return true;
}

DirectorEvent DirectorEventFromKillTelemetry(const ServerTelemetryKillEvent& kill, DWORD now) {
    DirectorEvent event = {};
    event.type = DirectorEventType::Kill;
    event.sequence = kill.sequence;
    event.rawTick = kill.receivedTick != 0 ? kill.receivedTick : (kill.eventTick != 0 ? kill.eventTick : now);
    event.visibleTick = now;
    event.killerHandle = kill.killerId;
    event.victimHandle = kill.victimId;
    event.weaponId = kill.weaponId;
    event.hasServerTelemetry = true;
    event.serverTelemetryFlags = kill.flags;
    CopyText(event.serverWeaponName, sizeof(event.serverWeaponName), kill.weaponName);
    return event;
}

DirectorEvent DirectorEventFromFlagTelemetry(const ServerTelemetryFlagEvent& flag, DWORD now) {
    DirectorEvent event = {};
    event.type = DirectorEventType::Flag;
    event.sequence = flag.sequence;
    event.rawTick = flag.receivedTick != 0 ? flag.receivedTick : (flag.eventTick != 0 ? flag.eventTick : now);
    event.visibleTick = now;
    event.hasFlagTelemetry = true;
    event.flagAction = flag.action;
    event.flagSide = flag.flagSide;
    event.flagReasonFlags = flag.flags;
    event.flagCarryTimeMs = flag.carryTimeMs;
    event.flagCarryDistanceMeters = flag.carryDistanceMeters;
    event.flagPlayerHandle = flag.playerId;
    event.flagPlayerTeam = flag.playerTeam;
    CopyText(event.flagPlayerName, sizeof(event.flagPlayerName), flag.playerName);
    return event;
}

bool QueueTargetKillTelemetryLocked(const CommentaryCameraContext& context,
                                    const ServerTelemetryKillEvent& kill,
                                    DWORD now) {
    int target = context.activeCameraTarget;
    if (target == 0 || kill.sequence == 0) return false;
    if (kill.killerId != target && kill.victimId != target) return false;
    if (g_liveKillLineSequences.find(kill.sequence) != g_liveKillLineSequences.end()) return false;

    DWORD eventTick = kill.receivedTick != 0 ? kill.receivedTick : kill.eventTick;
    if (!IsFreshEvent(now, eventTick, kLiveTargetEventFreshMs)) {
        g_liveKillLineSequences.insert(kill.sequence);
        return false;
    }
    if (!HappenedOnCurrentTargetView(eventTick)) {
        g_liveKillLineSequences.insert(kill.sequence);
        return false;
    }

    DirectorEvent event = DirectorEventFromKillTelemetry(kill, now);
    std::string text;
    int priority = 0;
    const char* nameSource = "none";
    const char* category = "target-kill";
    if (!BuildKillLine(event,
                       now,
                       target,
                       context.shotKind,
                       text,
                       priority,
                       &nameSource,
                       &category)) {
        return false;
    }

    bool suicide = (kill.flags & STREAMER_KILL_FLAG_SUICIDE) != 0;
    bool targetDied = kill.victimId == target;
    if (targetDied) {
        int minimumPriority = suicide ? 96 : 97;
        if (priority < minimumPriority) priority = minimumPriority;
    } else {
        if (priority < 91) priority = 91;
    }

    PendingLine line = {};
    line.pending = true;
    line.activationTick = targetDied
        ? LiveTelemetryVisibleEventTick(kill.receivedTick, kill.eventTick, now)
        : LiveTelemetryActivationTick(kill.receivedTick, kill.eventTick, now);
    line.expireTick = line.activationTick + kLiveTargetEventFreshMs;
    line.requiredTarget = target;
    line.priority = priority;
    line.subjectHandle = kill.killerId;
    line.objectHandle = kill.victimId;
    line.eventSequence = kill.sequence;
    line.cameraSwitchTolerant = targetDied;
    line.category = std::string("target-") + category;
    line.nameSource = nameSource;
    line.text = text;

    g_liveKillLineSequences.insert(kill.sequence);
    if (TickReached(now, line.activationTick)) {
        SetPendingLocked(line, now, "target-kill-telemetry");
    } else {
        QueueTargetBacklogLocked(line, now, "target-kill-telemetry-delay");
    }
    return true;
}

bool QueueTargetHitTelemetryLocked(const CommentaryCameraContext& context,
                                   const ServerTelemetryHitEvent& hit,
                                   DWORD now) {
    int target = context.activeCameraTarget;
    if (target == 0 || hit.sequence == 0) return false;
    if (hit.attackerId != target && hit.victimId != target) return false;
    if ((hit.flags & kHitFlagKill) != 0) return false;
    if (hit.damageHP < 15.0f) return false;
    if (g_liveHitLineSequences.find(hit.sequence) != g_liveHitLineSequences.end()) return false;

    DWORD eventTick = hit.receivedTick != 0 ? hit.receivedTick : hit.eventTick;
    if (!IsFreshEvent(now, eventTick, kLiveTargetEventFreshMs)) {
        g_liveHitLineSequences.insert(hit.sequence);
        return false;
    }
    if (!HappenedOnCurrentTargetView(eventTick)) {
        g_liveHitLineSequences.insert(hit.sequence);
        return false;
    }

    bool targetAttacker = hit.attackerId == target;
    ResolvedName attackerName = ResolveNameWithRoleFallback(hit.attackerId,
                                                            hit.attackerName,
                                                            "hit-telemetry",
                                                            hit.attackerTeam,
                                                            targetAttacker ? "attacker" : "defender",
                                                            hit.attackerId == target ? "watched player" : "attacker");
    ResolvedName victimName = ResolveNameWithRoleFallback(hit.victimId,
                                                          hit.victimName,
                                                          "hit-telemetry",
                                                          hit.victimTeam,
                                                          hit.victimId == target ? "defender" : "target",
                                                          hit.victimId == target ? "watched player" : "target");
    if (!attackerName.ok || !victimName.ok) return false;

    const char* weapon = hit.weaponName[0] ? hit.weaponName : nullptr;
    unsigned int damage = static_cast<unsigned int>(hit.damageHP + 0.5f);
    unsigned int distance = static_cast<unsigned int>(hit.distanceMeters + 0.5f);
    unsigned int variant = static_cast<unsigned int>((hit.sequence + static_cast<unsigned long long>(target)) % 4);
    bool targetIsFlagCarrier = target == context.usCarrier
        || target == context.vcCarrier
        || IsFlagShot(context.shotKind);

    char buffer[260] = {};
    if (targetIsFlagCarrier && targetAttacker) {
        if (weapon && distance >= 20) {
            sprintf_s(buffer,
                      "%s tags %s for %u HP with %s while carrying the flag. Multitasking like a lunatic.",
                      attackerName.name,
                      victimName.name,
                      damage,
                      weapon);
        } else {
            sprintf_s(buffer,
                      "%s hits %s for %u HP while carrying the flag. Rambo behavior, frankly.",
                      attackerName.name,
                      victimName.name,
                      damage);
        }
    } else if (targetIsFlagCarrier) {
        if (weapon) {
            sprintf_s(buffer,
                      "%s is attacked by %s while carrying the flag, eating %u HP from %s.",
                      victimName.name,
                      attackerName.name,
                      damage,
                      weapon);
        } else {
            sprintf_s(buffer,
                      "%s is attacked by %s while carrying the flag. %u HP gone, lovely.",
                      victimName.name,
                      attackerName.name,
                      damage);
        }
    } else if (targetAttacker) {
        if (weapon && distance >= 25) {
            sprintf_s(buffer,
                      "%s tags %s for %u HP with %s from %u m.",
                      attackerName.name,
                      victimName.name,
                      damage,
                      weapon,
                      distance);
        } else if (variant == 0) {
            sprintf_s(buffer,
                      "%s chunks %s for %u HP. Rude but productive.",
                      attackerName.name,
                      victimName.name,
                      damage);
        } else if (variant == 1) {
            sprintf_s(buffer,
                      "%s hits %s for %u HP. Keep bullying that health bar.",
                      attackerName.name,
                      victimName.name,
                      damage);
        } else {
            sprintf_s(buffer,
                      "%s lands %u damage on %s.",
                      attackerName.name,
                      damage,
                      victimName.name);
        }
    } else {
        if (weapon) {
            sprintf_s(buffer,
                      "%s just ate %u HP from %s's %s.",
                      victimName.name,
                      damage,
                      attackerName.name,
                      weapon);
        } else if (variant == 0) {
            sprintf_s(buffer,
                      "%s gets tagged for %u HP by %s. That one counted.",
                      victimName.name,
                      damage,
                      attackerName.name);
        } else {
            sprintf_s(buffer,
                      "%s hits %s for %u HP. Not ideal.",
                      attackerName.name,
                      victimName.name,
                      damage);
        }
    }

    PendingLine line = {};
    line.pending = true;
    line.activationTick = LiveTelemetryActivationTick(hit.receivedTick, hit.eventTick, now);
    line.expireTick = line.activationTick + kLiveTargetEventFreshMs;
    line.requiredTarget = target;
    line.priority = targetIsFlagCarrier ? (hit.damageHP >= 50.0f ? 95 : 92)
        : (hit.damageHP >= 50.0f ? 93 : (targetAttacker ? 87 : 89));
    line.subjectHandle = hit.attackerId;
    line.objectHandle = hit.victimId;
    line.eventSequence = hit.sequence;
    line.category = targetIsFlagCarrier
        ? (targetAttacker ? "target-flag-carrier-hit-dealt" : "target-flag-carrier-hit-taken")
        : (targetAttacker ? "target-hit-dealt" : "target-hit-taken");
    line.nameSource = targetAttacker ? attackerName.source : victimName.source;
    line.text = buffer;

    g_liveHitLineSequences.insert(hit.sequence);
    if (TickReached(now, line.activationTick)) {
        SetPendingLocked(line, now, "target-hit-telemetry");
    } else {
        QueueTargetBacklogLocked(line, now, "target-hit-telemetry-delay");
    }
    return true;
}

bool QueueTargetFlagTelemetryLocked(const CommentaryCameraContext& context,
                                    const ServerTelemetryFlagEvent& flag,
                                    DWORD now) {
    int target = context.activeCameraTarget;
    if (target == 0 || flag.sequence == 0 || flag.playerId != target) return false;
    if (g_liveFlagLineSequences.find(flag.sequence) != g_liveFlagLineSequences.end()) return false;

    DWORD eventTick = flag.receivedTick != 0 ? flag.receivedTick : flag.eventTick;
    if (!IsFreshEvent(now, eventTick, kLiveTargetEventFreshMs)) {
        g_liveFlagLineSequences.insert(flag.sequence);
        return false;
    }
    if (!HappenedOnCurrentTargetView(eventTick)) {
        g_liveFlagLineSequences.insert(flag.sequence);
        return false;
    }

    DirectorEvent event = DirectorEventFromFlagTelemetry(flag, now);
    std::string text;
    int priority = 0;
    const char* nameSource = "none";
    const char* category = "target-flag";
    if (!BuildFlagLine(event, text, priority, &nameSource, &category)) {
        return false;
    }

    PendingLine line = {};
    line.pending = true;
    line.activationTick = LiveTelemetryActivationTick(flag.receivedTick, flag.eventTick, now);
    line.expireTick = line.activationTick + kLiveTargetEventFreshMs;
    line.requiredTarget = target;
    line.priority = priority < 93 ? 93 : priority;
    line.subjectHandle = flag.playerId;
    line.eventSequence = flag.sequence;
    line.category = std::string("target-") + category;
    line.nameSource = nameSource;
    line.text = text;

    g_liveFlagLineSequences.insert(flag.sequence);
    if (TickReached(now, line.activationTick)) {
        SetPendingLocked(line, now, "target-flag-telemetry");
    } else {
        QueueTargetBacklogLocked(line, now, "target-flag-telemetry-delay");
    }
    return true;
}

bool QueueTargetAchievementTelemetryLocked(const CommentaryCameraContext& context,
                                           const ServerTelemetryAchievementEvent& achievement,
                                           DWORD now) {
    int target = context.activeCameraTarget;
    if (target == 0
        || achievement.sequence == 0
        || achievement.playerId != target
        || achievement.achievementId == 0) {
        return false;
    }
    if (g_liveAchievementLineSequences.find(achievement.sequence) != g_liveAchievementLineSequences.end()) {
        return false;
    }

    DWORD eventTick = achievement.receivedTick != 0 ? achievement.receivedTick : achievement.eventTick;
    if (!IsFreshEvent(now, eventTick, kLiveTargetEventFreshMs)) {
        g_liveAchievementLineSequences.insert(achievement.sequence);
        return false;
    }
    if (!HappenedOnCurrentTargetView(eventTick)) {
        g_liveAchievementLineSequences.insert(achievement.sequence);
        return false;
    }

    ResolvedName name = ResolveNameWithRoleFallback(target,
                                                    achievement.playerName,
                                                    "achievement-telemetry",
                                                    achievement.playerTeam,
                                                    "attacker",
                                                    "watched player");
    if (!name.ok) return false;

    char buffer[220] = {};
    if (achievement.achievementId == 1) {
        sprintf_s(buffer, "%s gets a double kill. The server noticed, unfortunately.", name.name);
    } else if (achievement.achievementId == 2) {
        sprintf_s(buffer, "%s gets a triple kill. Someone stop feeding him.", name.name);
    } else {
        sprintf_s(buffer, "%s triggers achievement %u. Fancy little badge moment.", name.name, achievement.achievementId);
    }

    PendingLine line = {};
    line.pending = true;
    line.activationTick = LiveTelemetryActivationTick(achievement.receivedTick, achievement.eventTick, now);
    line.expireTick = line.activationTick + kLiveTargetEventFreshMs;
    line.requiredTarget = target;
    line.priority = 95;
    line.subjectHandle = target;
    line.eventSequence = achievement.sequence;
    line.category = "target-achievement";
    line.nameSource = name.source;
    line.text = buffer;

    g_liveAchievementLineSequences.insert(achievement.sequence);
    if (TickReached(now, line.activationTick)) {
        SetPendingLocked(line, now, "target-achievement-telemetry");
    } else {
        QueueTargetBacklogLocked(line, now, "target-achievement-telemetry-delay");
    }
    return true;
}

bool QueueLiveTargetTelemetryLocked(const CommentaryCameraContext& context) {
    if (context.activeCameraTarget == 0) return false;

    DWORD now = context.currentTick;
    bool queued = false;

    std::vector<ServerTelemetryKillEvent> kills;
    ServerTelemetry_CopyKills(kills);
    std::sort(kills.begin(), kills.end(), [](const ServerTelemetryKillEvent& a, const ServerTelemetryKillEvent& b) {
        DWORD at = a.receivedTick != 0 ? a.receivedTick : a.eventTick;
        DWORD bt = b.receivedTick != 0 ? b.receivedTick : b.eventTick;
        if (at != bt) return at < bt;
        return a.sequence < b.sequence;
    });
    for (const ServerTelemetryKillEvent& kill : kills) {
        queued = QueueTargetKillTelemetryLocked(context, kill, now) || queued;
    }

    std::vector<ServerTelemetryHitEvent> hits;
    ServerTelemetry_CopyHits(hits);
    std::sort(hits.begin(), hits.end(), [](const ServerTelemetryHitEvent& a, const ServerTelemetryHitEvent& b) {
        DWORD at = a.receivedTick != 0 ? a.receivedTick : a.eventTick;
        DWORD bt = b.receivedTick != 0 ? b.receivedTick : b.eventTick;
        if (at != bt) return at < bt;
        return a.sequence < b.sequence;
    });
    for (const ServerTelemetryHitEvent& hit : hits) {
        queued = QueueTargetHitTelemetryLocked(context, hit, now) || queued;
    }

    std::vector<ServerTelemetryFlagEvent> flags;
    ServerTelemetry_CopyFlags(flags);
    std::sort(flags.begin(), flags.end(), [](const ServerTelemetryFlagEvent& a, const ServerTelemetryFlagEvent& b) {
        DWORD at = a.receivedTick != 0 ? a.receivedTick : a.eventTick;
        DWORD bt = b.receivedTick != 0 ? b.receivedTick : b.eventTick;
        if (at != bt) return at < bt;
        return a.sequence < b.sequence;
    });
    for (const ServerTelemetryFlagEvent& flag : flags) {
        queued = QueueTargetFlagTelemetryLocked(context, flag, now) || queued;
    }

    std::vector<ServerTelemetryAchievementEvent> achievements;
    ServerTelemetry_CopyAchievements(achievements);
    std::sort(achievements.begin(), achievements.end(), [](const ServerTelemetryAchievementEvent& a, const ServerTelemetryAchievementEvent& b) {
        DWORD at = a.receivedTick != 0 ? a.receivedTick : a.eventTick;
        DWORD bt = b.receivedTick != 0 ? b.receivedTick : b.eventTick;
        if (at != bt) return at < bt;
        return a.sequence < b.sequence;
    });
    for (const ServerTelemetryAchievementEvent& achievement : achievements) {
        queued = QueueTargetAchievementTelemetryLocked(context, achievement, now) || queued;
    }

    return queued;
}

bool TryQueueContextLineLocked(const CommentaryCameraContext& context) {
    DWORD now = context.currentTick;
    if (g_active.active || g_pending.pending || !CanStartLineLocked(now, 55)) {
        return false;
    }

    std::string text;
    const char* nameSource = "none";
    if (BuildFlagLoopLine(context, text, &nameSource)) {
        PendingLine line = {};
        line.pending = true;
        line.activationTick = now;
        line.expireTick = now + 1200;
        line.requiredTarget = context.activeCameraTarget;
        line.priority = 58;
        line.subjectHandle = context.activeCameraTarget;
        line.category = "flag-loop";
        line.nameSource = nameSource;
        line.text = text;
        SetPendingLocked(line, now, "camera-context");
        return true;
    }

    if (TryQueueGameStateLineLocked(context)) {
        return true;
    }

    if (context.activeCameraTarget == 0) return false;
    if (context.activeCameraTarget != g_lastContextTarget) {
        g_lastContextTarget = context.activeCameraTarget;
        g_targetSinceTick = now;
        return false;
    }
    if (g_targetSinceTick == 0) {
        g_targetSinceTick = now;
        return false;
    }
    if (!TickReached(now, g_targetSinceTick + kStatStableTargetMs)) {
        return false;
    }
    if (g_nextStatTick != 0 && !TickReached(now, g_nextStatTick)) {
        return false;
    }

    text.clear();
    nameSource = "none";
    if (!BuildStatLine(context.activeCameraTarget, text, &nameSource)) {
        g_nextStatTick = now + 5000;
        return false;
    }

    PendingLine line = {};
    line.pending = true;
    line.activationTick = now;
    line.expireTick = now + 1200;
    line.requiredTarget = context.activeCameraTarget;
    line.priority = 35;
    line.subjectHandle = context.activeCameraTarget;
    line.category = "stat-highlight";
    line.nameSource = nameSource;
    line.text = text;
    SetPendingLocked(line, now, "camera-context");
    g_nextStatTick = now + kStatMinIntervalMs + ((now / 113) % 5000);
    return true;
}

} // namespace

void CommentaryEngine_Init(uintptr_t gameBase) {
    PlayerIdentityCache_Init(gameBase);
    CommentaryEngine_Reset();
    CommentaryLog("initialized\n");
}

void CommentaryEngine_Reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active = ActiveLine();
    g_pending = PendingLine();
    g_targetBacklog.clear();
    g_lastGlobalKillLine = 0;
    g_lastGlobalLineStart = 0;
    g_nextStatTick = 0;
    g_nextStableScoreTick = 0;
    g_nextAggregateStatTick = 0;
    g_lastScoreCommentTick = 0;
    g_targetSinceTick = 0;
    g_lastContextTarget = 0;
    g_generalSequence = 1;
    g_teamScore = TeamScoreState();
    g_pendingScoreChange = PendingScoreChange();
    g_lastPlayerKillLine.clear();
    g_nextFlagLoopTick.clear();
    g_statCursor.clear();
    g_playerStates.clear();
    g_latestStats.clear();
    g_localKills.clear();
    g_localDeaths.clear();
    g_seenKillSequences.clear();
    g_liveKillLineSequences.clear();
    g_liveHitLineSequences.clear();
    g_liveFlagLineSequences.clear();
    g_liveAchievementLineSequences.clear();
    g_recentGeneralTemplates.clear();
    g_lastGeneralCategory.clear();
    g_lastTelemetryCacheTick = 0;
    g_lastCommentedKill = LastCommentedKill();
    g_lastContext = CommentaryCameraContext();
    g_lastPublishedVisible = false;
}

void CommentaryEngine_UpdateScoreboard(const std::vector<PlayerInfo>& players) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int usScore = 0;
    int vcScore = 0;
    bool hasTeamScore = false;

    for (const PlayerInfo& player : players) {
        if (player.teamId == 0) {
            usScore += player.score;
            hasTeamScore = true;
        } else if (player.teamId == 1) {
            vcScore += player.score;
            hasTeamScore = true;
        }
    }

    if (!hasTeamScore) return;

    DWORD now = GetTickCount();
    if (!g_teamScore.valid) {
        g_teamScore.valid = true;
        g_teamScore.us = usScore;
        g_teamScore.vc = vcScore;
        if (g_nextStableScoreTick == 0) {
            g_nextStableScoreTick = now + RandomDelay(now, kStableScoreMinIntervalMs, kStableScoreMaxIntervalMs);
        }
        if (g_nextAggregateStatTick == 0) {
            g_nextAggregateStatTick = now + RandomDelay(now + 41, kAggregateStatMinIntervalMs, kAggregateStatMaxIntervalMs);
        }
        return;
    }

    if (g_teamScore.us != usScore || g_teamScore.vc != vcScore) {
        g_pendingScoreChange.pending = true;
        g_pendingScoreChange.changeTick = now;
        g_pendingScoreChange.eligibleTick = now + RandomDelay(now + 7, kScoreCommentMinDelayMs, kScoreCommentMaxDelayMs);
        g_pendingScoreChange.expireTick = now + kScoreCommentMaxDelayMs;
        g_pendingScoreChange.oldUs = g_teamScore.us;
        g_pendingScoreChange.oldVc = g_teamScore.vc;
        g_pendingScoreChange.newUs = usScore;
        g_pendingScoreChange.newVc = vcScore;
        g_nextStableScoreTick = now + RandomDelay(now + 29, kStableScoreMinIntervalMs, kStableScoreMaxIntervalMs);
        CommentaryLog("score-change old=%d-%d new=%d-%d eligible=%lu expire=%lu\n",
                      g_teamScore.us,
                      g_teamScore.vc,
                      usScore,
                      vcScore,
                      g_pendingScoreChange.eligibleTick,
                      g_pendingScoreChange.expireTick);
    }

    g_teamScore.us = usScore;
    g_teamScore.vc = vcScore;
}

void CommentaryEngine_OnCommittedDirectorEvent(const DirectorEvent& event,
                                               DWORD now,
                                               int activeCameraTarget,
                                               const char* shotKind) {
    std::lock_guard<std::mutex> lock(g_mutex);
    PlayerIdentityCache_Poll();
    RefreshTelemetryCachesLocked(now);

    std::string text;
    int priority = 0;
    const char* nameSource = "none";
    const char* category = "event";
    bool built = event.type == DirectorEventType::Kill
        ? BuildKillLine(event, now, activeCameraTarget, shotKind, text, priority, &nameSource, &category)
        : BuildFlagLine(event, text, priority, &nameSource, &category);
    if (!built) {
        CommentaryLog("suppress seq=%llu type=%s target=%d cameraTarget=%d shot=%s reason=no-resolved-line\n",
                      event.sequence,
                      event.type == DirectorEventType::Kill ? "kill" : "flag",
                      activeCameraTarget,
                      g_lastContext.activeCameraTarget,
                      shotKind ? shotKind : "unknown");
        return;
    }

    bool flagCarrierKill = event.type == DirectorEventType::Kill
        && strcmp(category, "flag-carrier-kill") == 0;
    DWORD activationTick = flagCarrierKill
        ? now
        : (event.type == DirectorEventType::Kill ? event.visibleTick : now);
    DWORD lateDropMs = event.type == DirectorEventType::Kill ? kLateKillDropMs : kLateFlagDropMs;
    if (TickReached(now, activationTick + lateDropMs)) {
        CommentaryLog("drop seq=%llu category=%s target=%d activation=%lu now=%lu reason=late text=%s\n",
                      event.sequence,
                      category,
                      activeCameraTarget,
                      activationTick,
                      now,
                      text.c_str());
        return;
    }

    PendingLine line = {};
    line.pending = true;
    line.activationTick = activationTick;
    line.expireTick = activationTick + lateDropMs;
    line.requiredTarget = activeCameraTarget;
    line.priority = priority;
    line.subjectHandle = event.type == DirectorEventType::Kill ? event.killerHandle : event.flagPlayerHandle;
    line.objectHandle = event.type == DirectorEventType::Kill ? event.victimHandle : 0;
    line.eventSequence = event.sequence;
    line.category = category;
    line.nameSource = nameSource;
    line.text = text;

    if (event.type == DirectorEventType::Kill) {
        ServerTelemetryKillEvent kill = {};
        if (ServerTelemetry_TryFindKill(event.killerHandle, event.victimHandle, event.rawTick, &kill)
            && kill.sequence != 0) {
            g_liveKillLineSequences.insert(kill.sequence);
        }
    } else if (event.hasFlagTelemetry && event.flagPlayerHandle != 0) {
        ServerTelemetryFlagEvent flag = {};
        if (ServerTelemetry_TryFindFlag(event.flagPlayerHandle, event.flagAction, event.rawTick, &flag)
            && flag.sequence != 0) {
            g_liveFlagLineSequences.insert(flag.sequence);
        }
    }

    SetPendingLocked(line, now, nameSource);
}

void CommentaryEngine_Update(const CommentaryCameraContext& context) {
    std::lock_guard<std::mutex> lock(g_mutex);
    DWORD now = context.currentTick != 0 ? context.currentTick : GetTickCount();
    CommentaryCameraContext current = context;
    current.currentTick = now;
    g_lastContext = current;

    PlayerIdentityCache_Poll();
    RefreshTelemetryCachesLocked(now);

    if (g_lastContextTarget != current.activeCameraTarget) {
        g_lastContextTarget = current.activeCameraTarget;
        g_targetSinceTick = now;
    }

    if (g_active.active && !IsLineTargetStillActive(g_active, current.activeCameraTarget)) {
        ClearActiveLocked(now, "camera-target-changed");
    }

    if (g_active.active) {
        DWORD elapsed = now - g_active.startTick;
        if (elapsed >= kFadeInMs + kActiveHoldMs + kFadeOutMs) {
            ClearActiveLocked(now, "expired");
        }
    }

    QueueLiveTargetTelemetryLocked(current);

    if (g_pending.pending) {
        if (!IsLineTargetStillActive(g_pending, current.activeCameraTarget)) {
            CommentaryLog("drop-pending seq=%llu category=%s requiredTarget=%d cameraTarget=%d priority=%d source=%s shot=%s reason=off-camera text=%s\n",
                          g_pending.eventSequence,
                          g_pending.category.c_str(),
                          g_pending.requiredTarget,
                          current.activeCameraTarget,
                          g_pending.priority,
                          g_pending.nameSource.c_str(),
                          current.shotKind ? current.shotKind : "unknown",
                          g_pending.text.c_str());
            g_pending = PendingLine();
        } else if (TickReached(now, g_pending.expireTick)) {
            CommentaryLog("drop-pending seq=%llu category=%s activation=%lu expire=%lu now=%lu priority=%d source=%s reason=late text=%s\n",
                          g_pending.eventSequence,
                          g_pending.category.c_str(),
                          g_pending.activationTick,
                          g_pending.expireTick,
                          now,
                          g_pending.priority,
                          g_pending.nameSource.c_str(),
                          g_pending.text.c_str());
            g_pending = PendingLine();
        } else if (TickReached(now, g_pending.activationTick)
                   && !g_active.active
                   && CanStartLineLocked(now, g_pending.priority)) {
            PendingLine line = g_pending;
            g_pending = PendingLine();
            ActivatePendingLocked(line,
                                  now,
                                  current.activeCameraTarget,
                                  current.shotKind);
        }
    }

    PromoteTargetBacklogLocked(now, current.activeCameraTarget, current.shotKind);

    TryQueueContextLineLocked(current);

    if (g_pending.pending
        && TickReached(now, g_pending.activationTick)
        && !g_active.active
        && IsLineTargetStillActive(g_pending, current.activeCameraTarget)
        && CanStartLineLocked(now, g_pending.priority)) {
        PendingLine line = g_pending;
        g_pending = PendingLine();
        ActivatePendingLocked(line,
                              now,
                              current.activeCameraTarget,
                              current.shotKind);
    }

    PublishToVchdLocked(now);
}

bool CommentaryEngine_GetActiveLine(char* outText, size_t outTextLen, float* outAlpha) {
    if (outAlpha) *outAlpha = 0.0f;
    if (outText && outTextLen > 0) outText[0] = '\0';

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active.active || !outText || outTextLen == 0) {
        return false;
    }

    DWORD now = GetTickCount();
    float alpha = ActiveAlphaLocked(now);
    strncpy_s(outText, outTextLen, g_active.text.c_str(), _TRUNCATE);
    if (outAlpha) *outAlpha = alpha;
    return alpha > 0.001f;
}
