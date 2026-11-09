// presencepoller.h
//
// Copyright (c) 2026 Bing Lin
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.
//
// Personal fork feature: continuous background presence polling.
//
// While nchat runs, this polls the online/last-seen status of the most
// recent private contacts of each account and persists a local activity
// history on disk. Users whose status yields no observable data across a
// configured eligible-observation window (hidden status) are skipped.
//
// Persistence layout under <appdir>/presence/:
//   history.<hex-profile>.jsonl  append-only samples, heartbeats and markers
//   state                        per-user polling bookkeeping (atomic rewrite)
//
// Scheduling runs on monotonic time (random phase + +/-20% jitter, per-tick
// dispatch budget, re-phasing after long gaps). Record timestamps in the
// history are wall clock. Eligible observation time accrues per user only
// while the user is selected and the account can actually receive presence;
// it is capped per gap so downtime cannot fast-forward hidden detection.
//
// History integrity: appends are length-checked and rolled back on partial
// writes; if the rollback itself fails, the profile is blocked until the
// rollback succeeds, so retries can never duplicate records. Incomplete
// trailing lines from a crash are repaired before the next append. State is
// written only after its history lines are durable, and history records only
// genuine protocol observations plus explicit heartbeat re-affirmations
// ("hb":1) of the last known state; display-state guesses are never recorded.
// If the pending backlog overflows its hard cap, the oldest records are
// dropped (markers last) and the loss is reported; in that degraded mode the
// history-before-state guarantee is best-effort for the dropped records.
//
// Persistence runs on a private worker thread, so slow storage never blocks
// the UI thread. The worker owns all file handles and its failure state;
// the UI thread only hands off immutable batches.
//
// Header-only like localfeatures.h so standalone tests can build it without
// linking the full core. No core logging or protocol dependencies.

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PresencePoller
{
public:
  PresencePoller() = default;
  ~PresencePoller() { StopWorker(); }

  bool Init(const std::string& p_BaseDir, uint32_t p_RandomSeed = 0);
  bool IsInitialized() const { return m_Initialized; }
  bool HasPending() const { return !m_Pending.empty() || m_StateDirty || m_StateDirtySoft; }
  const std::string& GetError() const { return m_Error; }

  struct UserState
  {
    int64_t firstMs = 0;   // wall-clock time the user first entered the poll set
    int64_t eligibleMs = 0; // eligible observation time accumulated (monotonic)
    int64_t dataMs = 0;    // wall-clock time of the last observable data (0 = never)
    bool hidden = false;   // skip polling: no observable data across the window
  };

  // Scheduling tick, called with the current candidate list (most recent
  // private chats first, already truncated by the caller). p_SteadyMs must be
  // monotonic; p_WallMs is used only for stored timestamps. p_CanObserve is
  // false while the account is disconnected or announced offline; no eligible
  // time accrues then. Poll intervals follow an exponential gradient across
  // the candidate list: the most recent contact polls on p_FastIntervalMs
  // and the least recent on p_PollIntervalMs, so neighbouring ranks differ
  // less at the fast end than at the slow end (each with a random phase and
  // +/-20% jitter). Registers first-seen times, accumulates eligible time
  // for continuously selected users, marks never-observed users hidden
  // after p_HiddenDetectMs of eligible time, and returns the user ids due
  // for a poll request now (bounded by a small per-tick dispatch budget).
  std::vector<std::string> Tick(int64_t p_SteadyMs, int64_t p_WallMs,
                                const std::string& p_ProfileId,
                                const std::vector<std::string>& p_Candidates,
                                int64_t p_PollIntervalMs,
                                int64_t p_FastIntervalMs,
                                int64_t p_HiddenDetectMs,
                                bool p_CanObserve);

  // Exponential interpolation between the fast and slow intervals at a
  // candidate rank. Rank 0 returns the fast interval, the last rank the
  // slow interval; the gap between neighbouring ranks grows with the rank.
  static int64_t GradientInterval(int64_t p_FastIntervalMs,
                                  int64_t p_SlowIntervalMs,
                                  size_t p_Rank,
                                  size_t p_Count);

  // Record a genuine protocol observation (service-pushed update or poll
  // response). p_SteadyMs is monotonic (heartbeat cadence), p_WallMs is the
  // stored timestamp. Only candidate users are recorded; identical
  // consecutive observations are coalesced. Observable data clears a
  // previous hidden mark so the user returns to the poll set.
  void RecordStatus(int64_t p_SteadyMs, int64_t p_WallMs, const std::string& p_ProfileId,
                    const std::string& p_UserId, bool p_IsOnline,
                    int64_t p_TimeSeen, bool p_Recently);

  // Re-affirm the last recorded observation with a heartbeat line
  // ("hb":1). Does nothing for users with no observation yet. The heartbeat
  // gate uses p_SteadyMs; the stored line uses p_WallMs.
  void RecordHeartbeat(int64_t p_SteadyMs, int64_t p_WallMs,
                       const std::string& p_ProfileId, const std::string& p_UserId,
                       int64_t p_HeartbeatMs);

  // Hand pending history lines and a state snapshot to the persistence
  // worker. Returns true when the hand-off succeeded (durability is
  // asynchronous; failures surface through TakeWorkerError).
  bool Flush(bool p_ForceState = false);

  // Stop the persistence worker, draining what can be written with bounded
  // retries. Used on shutdown; the destructor calls it implicitly.
  void Shutdown();

  // Block until the persistence worker has no remaining work. Used by tests
  // and implicitly by the destructor.
  bool Drain();

  // Returns and clears the worker's last persistence error, if any.
  bool TakeWorkerError(std::string& p_Error);

  // Returns and clears the count of history lines dropped by the pending
  // queue overflow policy.
  size_t TakeDroppedLineCount();

  // Test hook: make the next rollback ftruncate fail, to exercise the
  // rollback-retry path. Only referenced by the regression test.
  static bool& TestFailTruncate();

private:
  struct LastSample
  {
    int64_t tsMs = 0;        // wall-clock time of the recorded observation
    bool online = false;
    int64_t timeSeen = -1;
    bool recently = false;
    int64_t emitSteadyMs = 0; // monotonic time of the last emission (heartbeat gate)
  };

  struct PendingLine
  {
    std::string profileId;
    std::string text;
    bool marker = false;
  };

  static const size_t s_MaxPendingLines = 20000;
  static const size_t s_MaxDispatchPerTick = 5;
  static const int64_t s_MaxEligibleDeltaMs = 5 * 60 * 1000; // cap per-tick gap
  static const int64_t s_StateSaveIntervalMs = 60 * 1000;    // soft-dirty checkpoint
  static const int64_t s_MinDispatchGapMs = 150; // floor between dispatch bursts

  static std::string EscapeJson(const std::string& p_Str);
  static std::string HexEncode(const std::string& p_Str);
  static std::string EncodeStateField(const std::string& p_Str);
  static std::string DecodeStateField(const std::string& p_Str);
  static bool ParseInt64(const std::string& p_Str, int64_t& p_Value);
  static int64_t SteadyNow();

  bool LoadState();
  std::string SerializeState() const;
  int64_t UniformInt(int64_t p_Lo, int64_t p_Hi);
  void QueueLine(const std::string& p_ProfileId, const std::string& p_Text,
                 bool p_Marker);

  void StartWorker();
  void StopWorker();
  void WorkerLoop();
  void EnforceQueueCapLocked(); // call with m_WorkMutex held
  void AddBlockedLines(const std::string& p_ProfileId,
                       const std::vector<PendingLine>& p_Lines);
  bool AppendHistory(const std::string& p_ProfileId,
                     const std::vector<std::string>& p_Lines,
                     std::string& p_Error,
                     bool& p_Blocked);
  bool SaveStateTo(const std::string& p_StateData, std::string& p_Error);
  bool SyncDir(std::string& p_Error);
  bool SyncParentDir(std::string& p_Error);
  static bool RepairIncompleteTail(int p_Fd, off_t& p_Length);

private:
  // UI-thread state.
  std::string m_Dir;
  std::string m_Error;
  bool m_Initialized = false;
  std::unordered_map<std::string, std::unordered_map<std::string, UserState>> m_UserStates;
  std::unordered_map<std::string, std::unordered_map<std::string, LastSample>> m_LastSamples;
  std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> m_NextPollMs;
  std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> m_ScheduledInterval;
  std::unordered_map<std::string, int64_t> m_LastDispatchMs;
  std::unordered_map<std::string, std::unordered_set<std::string>> m_Tracked;
  std::unordered_map<std::string, std::unordered_set<std::string>> m_PrevSelected;
  std::unordered_map<std::string, int64_t> m_LastTickMs;
  std::unordered_map<std::string, bool> m_LastObserved;
  std::vector<PendingLine> m_Pending;
  bool m_StateDirty = false;
  bool m_StateDirtySoft = false;
  int64_t m_LastStateSaveMs = 0;
  std::atomic<size_t> m_DroppedLines{0}; // UI thread and worker both bump this
  std::mt19937_64 m_Rng;

  // Persistence worker state (guarded by m_WorkMutex).
  std::thread m_Worker;
  std::mutex m_WorkMutex;
  std::condition_variable m_WorkCv;
  bool m_StopWorker = false;
  bool m_WorkerIdle = true;
  uint64_t m_WorkVersion = 0;
  uint64_t m_StateGeneration = 0;
  std::vector<PendingLine> m_WorkerQueue;
  std::string m_WorkerStateData;
  bool m_WorkerWriteState = false;
  std::string m_WorkerError;
  bool m_WorkerErrorPending = false;

  // Worker-owned failure bookkeeping.
  std::map<std::string, off_t> m_RollbackPending; // profile -> length to restore
  std::map<std::string, std::vector<PendingLine>> m_BlockedLines; // wait for rollback
  bool m_DirSyncNeeded = false;
  bool m_ParentSyncNeeded = false; // presence dir entry not yet durable in its parent
  std::string m_ParentDir;
};

inline bool& PresencePoller::TestFailTruncate()
{
  static bool value = false;
  return value;
}

inline int64_t PresencePoller::SteadyNow()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline int64_t PresencePoller::GradientInterval(int64_t p_FastIntervalMs,
                                                int64_t p_SlowIntervalMs,
                                                size_t p_Rank,
                                                size_t p_Count)
{
  if (p_FastIntervalMs < 1000) p_FastIntervalMs = 1000;
  if (p_SlowIntervalMs < p_FastIntervalMs) p_SlowIntervalMs = p_FastIntervalMs;
  if (p_Count <= 1 || p_FastIntervalMs == p_SlowIntervalMs)
  {
    return p_SlowIntervalMs;
  }
  const double t = static_cast<double>(p_Rank) / static_cast<double>(p_Count - 1);
  const double ratio = static_cast<double>(p_SlowIntervalMs) /
    static_cast<double>(p_FastIntervalMs);
  const double value = static_cast<double>(p_FastIntervalMs) * std::pow(ratio, t);
  return static_cast<int64_t>(std::llround(value));
}

inline bool PresencePoller::Init(const std::string& p_BaseDir, uint32_t p_RandomSeed)
{
  if (m_Initialized) return true; // idempotent: never reload over live state

  m_Dir = p_BaseDir;
  m_Error.clear();
  m_Initialized = false;

  {
    // The parent directory's entry for a newly created presence dir must
    // itself be made durable before persistence can claim crash safety.
    const size_t slash = m_Dir.find_last_of('/');
    m_ParentDir = (slash == std::string::npos) ? std::string(".") : m_Dir.substr(0, slash);
    if (m_ParentDir.empty()) m_ParentDir = "/";
  }

  if (mkdir(m_Dir.c_str(), 0700) == 0)
  {
    // Newly created: its entry in the parent is not yet durable.
    m_ParentSyncNeeded = true;
  }
  else if (errno != EEXIST)
  {
    m_Error = "cannot create presence dir " + m_Dir + ": " + std::to_string(errno);
    return false;
  }

  struct stat dirStat;
  if (lstat(m_Dir.c_str(), &dirStat) != 0 || !S_ISDIR(dirStat.st_mode))
  {
    // EEXIST did not establish a usable private directory; a symlink or
    // non-directory object here must not be written through.
    m_Error = "presence path is not a directory: " + m_Dir;
    return false;
  }

  if (p_RandomSeed != 0)
  {
    m_Rng.seed(p_RandomSeed);
  }
  else
  {
    m_Rng.seed(std::random_device{}());
  }

  if (!LoadState())
  {
    // An unreadable state file must not be overwritten by an empty
    // reconstruction; the poller stays inert until a retry succeeds.
    // Malformed individual lines are skipped; whole-file failures abort.
    return false;
  }

  StartWorker();
  m_Initialized = true;
  return true;
}

inline int64_t PresencePoller::UniformInt(int64_t p_Lo, int64_t p_Hi)
{
  if (p_Hi < p_Lo) return p_Lo;
  std::uniform_int_distribution<int64_t> dist(p_Lo, p_Hi);
  return dist(m_Rng);
}

inline std::vector<std::string> PresencePoller::Tick(int64_t p_SteadyMs, int64_t p_WallMs,
                                                     const std::string& p_ProfileId,
                                                     const std::vector<std::string>& p_Candidates,
                                                     int64_t p_PollIntervalMs,
                                                     int64_t p_FastIntervalMs,
                                                     int64_t p_HiddenDetectMs,
                                                     bool p_CanObserve)
{
  if (!m_Initialized) return std::vector<std::string>();

  if (p_PollIntervalMs < 1000) p_PollIntervalMs = 1000;
  if (p_FastIntervalMs < 1000) p_FastIntervalMs = 1000;
  if (p_FastIntervalMs > p_PollIntervalMs) p_FastIntervalMs = p_PollIntervalMs;

  // Interval gradient: rank 0 (most recent) polls at the fast interval and
  // the last candidate at the slow interval, interpolated exponentially.
  auto userInterval = [&](size_t p_Rank) -> int64_t {
    return GradientInterval(p_FastIntervalMs, p_PollIntervalMs, p_Rank,
                            p_Candidates.size());
  };

  auto& states = m_UserStates[p_ProfileId];
  bool dirty = false;
  bool softDirty = false;

  m_Tracked[p_ProfileId].clear();
  for (const std::string& userId : p_Candidates)
  {
    m_Tracked[p_ProfileId].insert(userId);
    UserState& st = states[userId];
    if (st.firstMs == 0)
    {
      st.firstMs = p_WallMs;
      dirty = true;
    }
  }

  // Eligible observation time for the elapsed gap. It accrues only when the
  // account could observe presence in BOTH the previous and the current
  // tick, so a false->true transition never credits the unobservable gap.
  // The cap keeps downtime from fast-forwarding the window.
  int64_t delta = 0;
  int64_t& lastTick = m_LastTickMs[p_ProfileId];
  const bool lastObserved = m_LastObserved[p_ProfileId];
  if (p_CanObserve && lastObserved && lastTick != 0 && p_SteadyMs >= lastTick)
  {
    delta = p_SteadyMs - lastTick;
    if (delta > s_MaxEligibleDeltaMs) delta = s_MaxEligibleDeltaMs;
  }
  lastTick = p_SteadyMs;
  m_LastObserved[p_ProfileId] = p_CanObserve;

  // Selected = non-hidden candidates. Eligible time accrues only to users
  // that were already selected in the previous tick, so newcomers never
  // inherit elapsed time from before they joined.
  auto& prev = m_PrevSelected[p_ProfileId];
  std::unordered_set<std::string> nowSelected;
  for (const std::string& userId : p_Candidates)
  {
    UserState& st = states[userId];
    if (st.hidden) continue;
    if (delta > 0 && prev.count(userId))
    {
      // Overflow-safe accumulation: saturate instead of wrapping.
      if (st.eligibleMs > INT64_MAX - delta)
      {
        st.eligibleMs = INT64_MAX;
      }
      else
      {
        st.eligibleMs += delta;
      }
      softDirty = true;
    }
    if (st.dataMs == 0 && p_HiddenDetectMs > 0 && st.eligibleMs >= p_HiddenDetectMs)
    {
      st.hidden = true;
      dirty = true;
      QueueLine(p_ProfileId,
                "{\"ts\":" + std::to_string(p_WallMs) +
                ",\"u\":\"" + EscapeJson(userId) + "\",\"mark\":\"hidden\"}",
                true /* marker */);
      continue;
    }
    nowSelected.insert(userId);
  }

  // Drop schedules for users no longer selected.
  auto& nextPoll = m_NextPollMs[p_ProfileId];
  auto& scheduled = m_ScheduledInterval[p_ProfileId];
  for (auto it = nextPoll.begin(); it != nextPoll.end(); )
  {
    if (!nowSelected.count(it->first))
    {
      scheduled.erase(it->first);
      it = nextPoll.erase(it);
    }
    else
    {
      ++it;
    }
  }

  // New users start at a random offset inside their tier's interval so the
  // whole set does not fire at once.
  std::unordered_map<std::string, size_t> rank;
  rank.reserve(p_Candidates.size());
  for (size_t i = 0; i < p_Candidates.size(); ++i)
  {
    rank[p_Candidates[i]] = i;
  }
  for (const std::string& userId : nowSelected)
  {
    if (!nextPoll.count(userId))
    {
      const int64_t interval = userInterval(rank[userId]);
      nextPoll[userId] = p_SteadyMs + UniformInt(0, interval - 1);
      scheduled[userId] = interval;
    }
  }

  // Dispatch only on ticks at least the minimum gap apart, so a burst of
  // overdue users cannot exceed s_MaxDispatchPerTick per gap window.
  std::vector<std::string> due;
  if (m_LastDispatchMs[p_ProfileId] == 0 ||
      p_SteadyMs - m_LastDispatchMs[p_ProfileId] >= s_MinDispatchGapMs)
  {
    // Users due now; reschedule with a +/-20% jitter around their tier's
    // interval. Long-overdue users are re-phased instead of firing a
    // catch-up burst, and only the per-tick dispatch budget is actually
    // fired and rescheduled; the rest stay overdue and fire on later
    // ticks. Promotion to a faster tier brings the deadline forward so a
    // contact that becomes active is sampled soon.
    for (const std::string& userId : nowSelected)
    {
      int64_t& t = nextPoll[userId];
      int64_t& sched = scheduled[userId];
      const int64_t interval = userInterval(rank[userId]);
      if (sched != 0 && interval < sched && t > p_SteadyMs)
      {
        t = std::min(t, p_SteadyMs + UniformInt(0, interval - 1));
      }
      sched = interval;
      if (t > p_SteadyMs) continue;
      if (t < p_SteadyMs - 2 * interval)
      {
        t = p_SteadyMs + UniformInt(0, interval - 1);
        continue;
      }
      if (due.size() >= s_MaxDispatchPerTick) break;
      due.push_back(userId);
      const int64_t jitter = interval / 5;
      t = p_SteadyMs + interval + UniformInt(-jitter, jitter);
      if (t <= p_SteadyMs) t = p_SteadyMs + 1000;
    }
    if (!due.empty())
    {
      m_LastDispatchMs[p_ProfileId] = p_SteadyMs;
    }
  }

  prev.swap(nowSelected);

  if (dirty) m_StateDirty = true;
  if (softDirty) m_StateDirtySoft = true;

  return due;
}

inline void PresencePoller::QueueLine(const std::string& p_ProfileId, const std::string& p_Text,
                                      bool p_Marker)
{
  m_Pending.push_back({ p_ProfileId, p_Text, p_Marker });
  if (m_Pending.size() > s_MaxPendingLines)
  {
    // Bounded pending data: drop the oldest non-marker lines first so state
    // decisions (hidden markers) are not silently discarded. If every line
    // is a marker, the oldest markers are dropped instead; the state
    // checkpoint still carries the decision, and the loss is reported.
    const size_t overflow = m_Pending.size() - s_MaxPendingLines;
    size_t dropped = 0;
    for (auto it = m_Pending.begin(); it != m_Pending.end() && dropped < overflow; )
    {
      if (!it->marker)
      {
        it = m_Pending.erase(it);
        ++dropped;
      }
      else
      {
        ++it;
      }
    }
    if (dropped < overflow)
    {
      const size_t remaining = overflow - dropped;
      m_Pending.erase(m_Pending.begin(), m_Pending.begin() + static_cast<long>(remaining));
      m_DroppedLines += remaining;
    }
    if (dropped > 0)
    {
      m_DroppedLines += dropped;
    }
  }
}

inline void PresencePoller::RecordStatus(int64_t p_SteadyMs, int64_t p_WallMs,
                                         const std::string& p_ProfileId,
                                         const std::string& p_UserId, bool p_IsOnline,
                                         int64_t p_TimeSeen, bool p_Recently)
{
  if (!m_Initialized) return;

  // Only users in the current poll candidate set are recorded; service
  // pushes for other chats update the UI but stay out of the history. The
  // one exception is a previously hidden user: its hidden mark must clear
  // when real data arrives even while its chat ranks outside the poll set,
  // otherwise a WhatsApp contact (unsubscribed while hidden) could never
  // recover.
  const auto trackedIt = m_Tracked.find(p_ProfileId);
  const bool tracked = (trackedIt != m_Tracked.end()) && trackedIt->second.count(p_UserId);
  if (!tracked)
  {
    bool recoverable = false;
    const auto stateIt = m_UserStates.find(p_ProfileId);
    if (stateIt != m_UserStates.end())
    {
      const auto userIt = stateIt->second.find(p_UserId);
      recoverable = (userIt != stateIt->second.end()) && userIt->second.hidden;
    }
    if (!recoverable) return;
  }

  UserState& st = m_UserStates[p_ProfileId][p_UserId];
  if (st.firstMs == 0)
  {
    st.firstMs = p_WallMs;
    m_StateDirty = true;
  }

  const bool hasData = p_IsOnline || (p_TimeSeen >= 0) || p_Recently;
  bool recovered = false;
  if (hasData)
  {
    if (st.dataMs != p_WallMs)
    {
      st.dataMs = p_WallMs;
      m_StateDirtySoft = true;
    }
    if (st.hidden)
    {
      // Observable data after a hidden mark: the peer's status is visible
      // again, so it returns to the poll set. The recovery observation must
      // be written even when it is identical to the last pre-hidden sample,
      // so consumers of the history can see the mark being retired.
      st.hidden = false;
      m_StateDirty = true;
      recovered = true;
    }
  }

  LastSample& last = m_LastSamples[p_ProfileId][p_UserId];
  if (!recovered && last.tsMs != 0 &&
      last.online == p_IsOnline &&
      last.timeSeen == p_TimeSeen &&
      last.recently == p_Recently)
  {
    return; // identical consecutive observation
  }

  // A fresh observation also resets the heartbeat cadence.
  last = { p_WallMs, p_IsOnline, p_TimeSeen, p_Recently, p_SteadyMs };

  std::string line = "{\"ts\":" + std::to_string(p_WallMs) +
    ",\"u\":\"" + EscapeJson(p_UserId) + "\"" +
    ",\"on\":" + (p_IsOnline ? "1" : "0") +
    ",\"seen\":" + std::to_string(p_TimeSeen) +
    ",\"recent\":" + (p_Recently ? "1" : "0") + "}";
  QueueLine(p_ProfileId, line, false /* marker */);
}

inline void PresencePoller::RecordHeartbeat(int64_t p_SteadyMs, int64_t p_WallMs,
                                            const std::string& p_ProfileId,
                                            const std::string& p_UserId,
                                            int64_t p_HeartbeatMs)
{
  if (!m_Initialized) return;
  if (p_HeartbeatMs < 1000) p_HeartbeatMs = 1000;

  LastSample& last = m_LastSamples[p_ProfileId][p_UserId];
  if (last.tsMs == 0)
  {
    return; // nothing observed yet; never fabricate a sample
  }
  if (p_SteadyMs - last.emitSteadyMs < p_HeartbeatMs)
  {
    return;
  }
  last.emitSteadyMs = p_SteadyMs;

  std::string line = "{\"ts\":" + std::to_string(p_WallMs) +
    ",\"u\":\"" + EscapeJson(p_UserId) + "\"" +
    ",\"on\":" + (last.online ? "1" : "0") +
    ",\"seen\":" + std::to_string(last.timeSeen) +
    ",\"recent\":" + (last.recently ? "1" : "0") +
    ",\"hb\":1}";
  QueueLine(p_ProfileId, line, false /* marker */);
}

inline bool PresencePoller::Flush(bool p_ForceState)
{
  if (!m_Initialized) return false;

  const bool hard = m_StateDirty;
  const bool soft = m_StateDirtySoft;
  if (m_Pending.empty() && !hard && !soft) return true;

  // Serialize a state snapshot when something hard changed or when the
  // soft-dirty checkpoint interval has passed. Eligible-time accumulation
  // is deliberately not saved on every tick.
  std::string stateData;
  const int64_t steady = SteadyNow();
  if (hard || p_ForceState || (soft && steady - m_LastStateSaveMs >= s_StateSaveIntervalMs))
  {
    stateData = SerializeState();
    m_LastStateSaveMs = steady;
    m_StateDirty = false;
    m_StateDirtySoft = false;
  }

  // The version bump (which wakes the worker's failure backoff) must happen
  // only when THIS call actually transfers work, never because the worker's
  // own retained retry lines still sit in the queue.
  const bool transferred = !m_Pending.empty() || !stateData.empty();
  {
    std::unique_lock<std::mutex> lock(m_WorkMutex);
    if (!m_Pending.empty())
    {
      m_WorkerQueue.insert(m_WorkerQueue.end(), m_Pending.begin(), m_Pending.end());
      m_Pending.clear();
      // The worker queue must stay bounded too, or a persistently failing
      // device would grow it without limit.
      EnforceQueueCapLocked();
    }
    if (!stateData.empty())
    {
      m_WorkerStateData = stateData;
      m_WorkerWriteState = true;
      ++m_StateGeneration; // newer snapshot supersedes any in-flight save
    }
    if (transferred)
    {
      ++m_WorkVersion;
      m_WorkerIdle = false;
      m_WorkCv.notify_one();
    }
  }

  return true;
}

inline void PresencePoller::Shutdown()
{
  if (!m_Initialized) return;

  Flush(true /* force state checkpoint */);

  {
    std::unique_lock<std::mutex> lock(m_WorkMutex);
    m_StopWorker = true;
    m_WorkCv.notify_all();
  }

  if (m_Worker.joinable())
  {
    m_Worker.join();
  }

  // The failure-tolerant exit path inside WorkerLoop drains what it can;
  // anything it could not write is reported through the worker error.
  std::string workerError;
  if (TakeWorkerError(workerError))
  {
    // Reported by the caller; the error string is non-empty.
    m_Error = workerError;
  }
}

inline bool PresencePoller::Drain()
{
  std::unique_lock<std::mutex> lock(m_WorkMutex);
  m_WorkCv.wait(lock, [&] { return m_WorkerIdle; });
  return !m_WorkerErrorPending;
}

inline bool PresencePoller::TakeWorkerError(std::string& p_Error)
{
  std::unique_lock<std::mutex> lock(m_WorkMutex);
  if (!m_WorkerErrorPending) return false;
  p_Error = m_WorkerError;
  m_WorkerError.clear();
  m_WorkerErrorPending = false;
  return true;
}

inline size_t PresencePoller::TakeDroppedLineCount()
{
  return m_DroppedLines.exchange(0);
}

inline void PresencePoller::StartWorker()
{
  // Guard against double initialization (L4): Init is only called again
  // after a failed attempt, but never start a second thread over a joinable
  // one.
  if (m_Worker.joinable()) return;
  m_Worker = std::thread([this] { WorkerLoop(); });
}

inline void PresencePoller::StopWorker()
{
  {
    std::unique_lock<std::mutex> lock(m_WorkMutex);
    m_StopWorker = true;
    m_WorkCv.notify_all();
  }
  if (m_Worker.joinable())
  {
    m_Worker.join();
  }
}

inline void PresencePoller::WorkerLoop()
{
  for (;;)
  {
    bool work = false;
    uint64_t workVersion = 0;
    {
      std::unique_lock<std::mutex> lock(m_WorkMutex);
      m_WorkCv.wait(lock, [&] {
        return m_StopWorker || !m_WorkerQueue.empty() || m_WorkerWriteState ||
               m_DirSyncNeeded || m_ParentSyncNeeded || !m_RollbackPending.empty();
      });
      if (m_StopWorker && m_WorkerQueue.empty() && !m_WorkerWriteState &&
          m_RollbackPending.empty() && !m_DirSyncNeeded && !m_ParentSyncNeeded)
      {
        if (!m_BlockedLines.empty())
        {
          // Held lines whose rollback never resolved cannot be written
          // safely; report the loss instead of exiting silently.
          size_t lost = 0;
          for (const auto& blocked : m_BlockedLines)
          {
            lost += blocked.second.size();
          }
          m_WorkerError = "presence persistence: " + std::to_string(lost) +
            " lines lost at shutdown (unresolved rollback)";
          m_WorkerErrorPending = true;
          m_BlockedLines.clear();
        }
        m_WorkerIdle = true;
        m_WorkCv.notify_all();
        return;
      }
      workVersion = m_WorkVersion;
      work = true;
    }

    if (!work) continue;

    bool failed = false;
    std::string passError;

    // (1) Retry pending rollbacks: partial appends whose truncation failed
    // must be restored before anything else can be written to that profile.
    if (!m_RollbackPending.empty())
    {
      for (auto it = m_RollbackPending.begin(); it != m_RollbackPending.end(); )
      {
        const std::string path = m_Dir + "/history." + HexEncode(it->first) + ".jsonl";
        bool restored = false;
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0)
        {
          if (ftruncate(fd, it->second) == 0)
          {
            restored = true;
          }
          close(fd);
        }
        if (restored)
        {
          it = m_RollbackPending.erase(it);
        }
        else
        {
          ++it;
          failed = true;
          passError = "cannot restore rolled-back presence history: " + std::to_string(errno);
        }
      }
    }

    // (2) Fold in blocked lines whose rollback has since succeeded. They are
    // older than anything in the queue, so they go to the FRONT of this
    // pass's batch, before any newer lines are appended, preserving
    // per-profile timestamp order.
    std::vector<PendingLine> releasedLines;
    if (!m_BlockedLines.empty())
    {
      for (auto it = m_BlockedLines.begin(); it != m_BlockedLines.end(); )
      {
        if (m_RollbackPending.count(it->first))
        {
          ++it;
          continue;
        }
        releasedLines.insert(releasedLines.end(), it->second.begin(), it->second.end());
        it = m_BlockedLines.erase(it);
      }
    }

    // (3) Take a private copy of the queue and state snapshot so the UI
    // thread can keep filling without touching worker-owned data. The state
    // buffer is COPIED, not swapped: it stays intact until the save
    // succeeds, so a transient failure can never write an empty state file.
    std::vector<PendingLine> lines;
    std::string stateData;
    bool writeState = false;
    uint64_t stateGeneration = 0;
    {
      std::unique_lock<std::mutex> lock(m_WorkMutex);
      lines.swap(m_WorkerQueue);
      writeState = m_WorkerWriteState;
      stateGeneration = m_StateGeneration;
      if (writeState)
      {
        stateData = m_WorkerStateData;
      }
    }

    if (!releasedLines.empty())
    {
      lines.insert(lines.begin(), releasedLines.begin(), releasedLines.end());
    }

    // Append history per profile. A profile whose rollback could not be
    // restored stays blocked: its lines wait for the rollback to succeed, so
    // a retry can never duplicate already-written records.
    if (!lines.empty())
    {
      std::map<std::string, std::vector<PendingLine>> batches;
      for (const PendingLine& line : lines)
      {
        batches[line.profileId].push_back(line);
      }
      for (const auto& batch : batches)
      {
        if (m_RollbackPending.count(batch.first))
        {
          AddBlockedLines(batch.first, batch.second);
          continue;
        }
        std::vector<std::string> texts;
        texts.reserve(batch.second.size());
        for (const PendingLine& line : batch.second)
        {
          texts.push_back(line.text);
        }
        bool profileBlocked = false;
        std::string batchError;
        if (!AppendHistory(batch.first, texts, batchError, profileBlocked))
        {
          failed = true;
          passError = batchError;
          if (profileBlocked)
          {
            AddBlockedLines(batch.first, batch.second);
          }
          else
          {
            // Re-queue this batch at the front, preserving order and the
            // marker flag of each line, then re-apply the queue cap so
            // failure retries cannot grow the backlog without bound.
            std::unique_lock<std::mutex> lock(m_WorkMutex);
            m_WorkerQueue.insert(m_WorkerQueue.begin(), batch.second.begin(), batch.second.end());
            EnforceQueueCapLocked();
          }
        }
        else if (!batchError.empty())
        {
          // The batch is durable; surface a deferred close diagnostic
          // without retrying the append.
          std::unique_lock<std::mutex> lock(m_WorkMutex);
          if (!m_WorkerErrorPending)
          {
            m_WorkerError = batchError;
            m_WorkerErrorPending = true;
          }
        }
      }
    }

    // (4) Directory durability for newly created history files.
    if (m_DirSyncNeeded && !failed)
    {
      if (SyncDir(passError))
      {
        m_DirSyncNeeded = false;
      }
      else
      {
        failed = true;
      }
    }

    // (4b) Parent-directory durability for a newly (re)created presence
    // directory: without it, its entry could vanish on crash.
    if (m_ParentSyncNeeded && !failed)
    {
      if (SyncParentDir(passError))
      {
        m_ParentSyncNeeded = false;
      }
      else
      {
        failed = true;
      }
    }

    // (5) State only after ALL history lines are durable, so a hidden
    // decision can never be persisted without its marker line. Lines that
    // were just released from a blocked profile are folded into this pass's
    // batch (appended in step 3); a failure there skips this. A newer
    // snapshot handed over while this one was in flight stays pending.
    if (writeState && !failed && m_BlockedLines.empty() && m_RollbackPending.empty())
    {
      bool queueEmpty = false;
      {
        std::unique_lock<std::mutex> lock(m_WorkMutex);
        queueEmpty = m_WorkerQueue.empty();
      }
      if (queueEmpty)
      {
        if (SaveStateTo(stateData, passError))
        {
          std::unique_lock<std::mutex> lock(m_WorkMutex);
          if (m_StateGeneration == stateGeneration)
          {
            // Only retire the snapshot the worker actually saved; a newer
            // one installed meanwhile must stay pending.
            m_WorkerWriteState = false;
            m_WorkerStateData.clear();
          }
        }
        else
        {
          failed = true;
        }
      }
    }

    if (failed)
    {
      std::unique_lock<std::mutex> lock(m_WorkMutex);
      if (!m_WorkerErrorPending)
      {
        m_WorkerError = passError.empty() ? std::string("presence persistence failed") : passError;
        m_WorkerErrorPending = true;
      }
      if (m_StopWorker && !m_BlockedLines.empty())
      {
        // The blocked-line loss count must be reported even when an earlier
        // unconsumed error is still pending; append it to whatever is there.
        size_t lost = 0;
        for (const auto& blocked : m_BlockedLines)
        {
          lost += blocked.second.size();
        }
        m_WorkerError += " (" + std::to_string(lost) + " blocked lines lost at shutdown)";
        m_BlockedLines.clear();
      }
      m_WorkerIdle = false;
      if (m_StopWorker)
      {
        // Shutdown must not retry a persistently failing device forever;
        // remaining lines are lost and reported through the pending error.
        m_WorkerIdle = true;
        m_WorkCv.notify_all();
        return;
      }
      // Back off so a persistently failing device is not hammered. The wait
      // ends early only when the UI thread hands over NEW work (a version
      // bump), never because the retry queue itself is non-empty.
      m_WorkCv.wait_for(lock, std::chrono::milliseconds(5000), [&] {
        return m_StopWorker || m_WorkVersion != workVersion;
      });
    }
    else
    {
      std::unique_lock<std::mutex> lock(m_WorkMutex);
      if (m_WorkerQueue.empty() && !m_WorkerWriteState && m_RollbackPending.empty() &&
          !m_DirSyncNeeded && !m_ParentSyncNeeded)
      {
        m_WorkerIdle = true;
        m_WorkCv.notify_all();
      }
    }
  }
}

inline void PresencePoller::EnforceQueueCapLocked()
{
  // Drop the oldest non-marker lines first (markers carry hidden decisions),
  // then oldest markers. The loss is reported through the dropped counter.
  while (m_WorkerQueue.size() > s_MaxPendingLines)
  {
    auto dropIt = m_WorkerQueue.end();
    for (auto it = m_WorkerQueue.begin(); it != m_WorkerQueue.end(); ++it)
    {
      if (!it->marker)
      {
        dropIt = it;
        break;
      }
    }
    if (dropIt == m_WorkerQueue.end())
    {
      dropIt = m_WorkerQueue.begin();
    }
    m_WorkerQueue.erase(dropIt);
    ++m_DroppedLines;
  }
}

inline void PresencePoller::AddBlockedLines(const std::string& p_ProfileId,
                                            const std::vector<PendingLine>& p_Lines)
{
  // Worker-owned: no lock needed for the map. The blocked set must stay
  // bounded too, or repeated rollback failures would grow it without limit.
  // Eviction prefers non-marker records (markers carry hidden decisions)
  // and picks the first candidate in profile order, not a global age order;
  // the loss is reported through the dropped counter.
  m_BlockedLines[p_ProfileId].insert(m_BlockedLines[p_ProfileId].end(),
                                     p_Lines.begin(), p_Lines.end());
  size_t total = 0;
  for (const auto& blocked : m_BlockedLines)
  {
    total += blocked.second.size();
  }
  while (total > s_MaxPendingLines)
  {
    // Find the oldest droppable line: first non-marker across profiles in
    // insertion order, else the oldest line of the first profile.
    auto dropProfile = m_BlockedLines.end();
    auto dropIt = std::vector<PendingLine>::iterator();
    bool foundMarker = false;
    for (auto it = m_BlockedLines.begin(); it != m_BlockedLines.end(); ++it)
    {
      if (it->second.empty()) continue;
      for (auto line = it->second.begin(); line != it->second.end(); ++line)
      {
        if (!line->marker)
        {
          dropProfile = it;
          dropIt = line;
          foundMarker = false;
          break;
        }
        if (!foundMarker)
        {
          foundMarker = true;
          dropProfile = it;
          dropIt = line;
        }
      }
      if (dropProfile != m_BlockedLines.end() && !foundMarker) break;
    }
    if (dropProfile == m_BlockedLines.end()) break;
    dropProfile->second.erase(dropIt);
    --total;
    ++m_DroppedLines; // atomic; reported by the UI thread
  }
}

inline bool PresencePoller::AppendHistory(const std::string& p_ProfileId,
                                          const std::vector<std::string>& p_Lines,
                                          std::string& p_Error,
                                          bool& p_Blocked)
{
  p_Blocked = false;
  const std::string path = m_Dir + "/history." + HexEncode(p_ProfileId) + ".jsonl";

  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  if (fd < 0)
  {
    // Recreate the directory in case it was removed while running.
    if (mkdir(m_Dir.c_str(), 0700) == 0)
    {
      m_ParentSyncNeeded = true; // the recreated entry needs parent durability
      fd = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    }
    else if (errno == EEXIST)
    {
      fd = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    }
  }
  if (fd < 0)
  {
    p_Error = "cannot open presence history " + path + ": " + std::to_string(errno);
    return false;
  }

  struct stat fileStat;
  if (fstat(fd, &fileStat) != 0 || !S_ISREG(fileStat.st_mode))
  {
    p_Error = "presence history is not a regular file: " + path;
    close(fd);
    return false;
  }
  if (fchmod(fd, 0600) != 0)
  {
    p_Error = "cannot set presence history permissions on " + path + ": " + std::to_string(errno);
    close(fd);
    return false;
  }

  const bool created = (fileStat.st_size == 0);

  off_t preLen = lseek(fd, 0, SEEK_END);
  if (preLen < 0 || !RepairIncompleteTail(fd, preLen))
  {
    p_Error = "cannot prepare presence history " + path + ": " + std::to_string(errno);
    close(fd);
    return false;
  }

  std::string data;
  for (const std::string& text : p_Lines)
  {
    data += text;
    data += "\n";
  }

  bool failure = false;
  size_t offset = 0;
  while (offset < data.size())
  {
    ssize_t rv = write(fd, data.data() + offset, data.size() - offset);
    if (rv < 0)
    {
      if (errno == EINTR) continue;
      failure = true;
      break;
    }
    if (rv == 0)
    {
      failure = true;
      break;
    }
    offset += static_cast<size_t>(rv);
  }

  if (!failure && fsync(fd) != 0)
  {
    failure = true;
  }

  if (failure)
  {
    // Roll back the partial append so a retry cannot duplicate records or
    // leave a malformed trailing fragment. If the rollback itself fails the
    // profile is blocked until it can be restored.
    if (TestFailTruncate() || ftruncate(fd, preLen) != 0)
    {
      p_Error = "cannot roll back partial presence history append on " + path +
        ": " + std::to_string(errno);
      close(fd);
      m_RollbackPending[p_ProfileId] = preLen;
      p_Blocked = true;
      return false;
    }
    p_Error = "cannot write presence history " + path + ": " + std::to_string(errno);
    close(fd);
    return false;
  }

  if (created)
  {
    // The directory entry must be synced whether or not the close below
    // succeeds; set the flag before the close handling.
    m_DirSyncNeeded = true;
  }

  if (close(fd) != 0)
  {
    // Rare deferred close failure: the data is already fsynced, so retrying
    // could duplicate lines. Report and treat the batch as written.
    p_Error = "close failed on presence history " + path + ": " + std::to_string(errno);
    return true;
  }

  return true;
}

inline bool PresencePoller::SyncDir(std::string& p_Error)
{
  const int dirFd = open(m_Dir.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (dirFd < 0)
  {
    p_Error = "cannot open presence dir for sync: " + std::to_string(errno);
    return false;
  }
  if (fsync(dirFd) != 0)
  {
    p_Error = "cannot fsync presence dir: " + std::to_string(errno);
    close(dirFd);
    return false;
  }
  close(dirFd);
  return true;
}

inline bool PresencePoller::SyncParentDir(std::string& p_Error)
{
  const int parentFd = open(m_ParentDir.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (parentFd < 0)
  {
    p_Error = "cannot open presence parent dir for sync: " + std::to_string(errno);
    return false;
  }
  if (fsync(parentFd) != 0)
  {
    p_Error = "cannot fsync presence parent dir: " + std::to_string(errno);
    close(parentFd);
    return false;
  }
  close(parentFd);
  return true;
}

inline bool PresencePoller::RepairIncompleteTail(int p_Fd, off_t& p_Length)
{
  if (p_Length == 0) return true;

  if (lseek(p_Fd, p_Length - 1, SEEK_SET) < 0) return false;
  char lastChar = 0;
  if (read(p_Fd, &lastChar, 1) != 1) return false;
  if (lastChar == '\n') return true; // complete tail

  // A crash during a previous append left a partial line: truncate back to
  // the last newline, scanning backwards in bounded chunks so a fragment
  // larger than one chunk cannot erase valid complete lines.
  const off_t chunkSize = 65536;
  off_t scanEnd = p_Length;
  while (scanEnd > 0)
  {
    const off_t scanStart = (scanEnd > chunkSize) ? (scanEnd - chunkSize) : 0;
    const size_t scanSize = static_cast<size_t>(scanEnd - scanStart);
    std::vector<char> buffer(scanSize);
    if (lseek(p_Fd, scanStart, SEEK_SET) < 0) return false;
    size_t readOffset = 0;
    while (readOffset < scanSize)
    {
      ssize_t rv = read(p_Fd, buffer.data() + readOffset, scanSize - readOffset);
      if (rv < 0)
      {
        if (errno == EINTR) continue;
        return false;
      }
      if (rv == 0) break;
      readOffset += static_cast<size_t>(rv);
    }

    for (off_t i = static_cast<off_t>(readOffset) - 1; i >= 0; --i)
    {
      if (buffer[static_cast<size_t>(i)] == '\n')
      {
        const off_t cutOffset = scanStart + i + 1;
        if (ftruncate(p_Fd, cutOffset) != 0) return false;
        p_Length = cutOffset;
        return true;
      }
    }

    if (scanStart == 0) break;
    scanEnd = scanStart;
  }

  // No complete line exists anywhere; drop everything.
  if (ftruncate(p_Fd, 0) != 0) return false;
  p_Length = 0;
  return true;
}

inline bool PresencePoller::LoadState()
{
  const std::string path = m_Dir + "/state";
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
  {
    if (errno == ENOENT) return true; // fresh install: empty state
    m_Error = "cannot open presence state " + path + ": " + std::to_string(errno);
    return false;
  }

  struct stat fileStat;
  if (fstat(fd, &fileStat) != 0 || !S_ISREG(fileStat.st_mode))
  {
    m_Error = "presence state is not a regular file: " + path;
    close(fd);
    return false;
  }

  std::string data;
  char buffer[4096];
  ssize_t rv = 0;
  while ((rv = read(fd, buffer, sizeof(buffer))) != 0)
  {
    if (rv < 0)
    {
      if (errno == EINTR) continue;
      m_Error = "cannot read presence state " + path + ": " + std::to_string(errno);
      close(fd);
      return false;
    }
    data.append(buffer, static_cast<size_t>(rv));
  }
  close(fd);

  // Parse into a temporary map; the live state is replaced only when the
  // whole file has been read successfully. Malformed individual lines are
  // skipped and dropped on the next save.
  std::unordered_map<std::string, std::unordered_map<std::string, UserState>> parsed;

  size_t start = 0;
  while (start < data.size())
  {
    size_t end = data.find('\n', start);
    if (end == std::string::npos) end = data.size();
    const std::string line = data.substr(start, end - start);
    start = end + 1;
    if (line.empty()) continue;

    std::vector<std::string> fields;
    size_t fieldStart = 0;
    for (;;)
    {
      size_t tab = line.find('\t', fieldStart);
      if (tab == std::string::npos)
      {
        fields.push_back(line.substr(fieldStart));
        break;
      }
      fields.push_back(line.substr(fieldStart, tab - fieldStart));
      fieldStart = tab + 1;
    }
    if (fields.size() != 6) continue; // malformed line

    int64_t firstMs = 0;
    int64_t eligibleMs = 0;
    int64_t dataMs = 0;
    if (!ParseInt64(fields[2], firstMs) ||
        !ParseInt64(fields[3], eligibleMs) ||
        !ParseInt64(fields[4], dataMs)) continue;
    if (firstMs <= 0 || eligibleMs < 0 || dataMs < 0) continue; // domain validation
    if (fields[5] != "0" && fields[5] != "1") continue;

    UserState st;
    st.firstMs = firstMs;
    st.eligibleMs = eligibleMs;
    st.dataMs = dataMs;
    st.hidden = (fields[5] == "1");

    parsed[DecodeStateField(fields[0])][DecodeStateField(fields[1])] = st;
  }

  m_UserStates.swap(parsed);
  return true;
}

inline std::string PresencePoller::SerializeState() const
{
  std::string data;
  for (const auto& profile : m_UserStates)
  {
    for (const auto& user : profile.second)
    {
      data += EncodeStateField(profile.first) + "\t" + EncodeStateField(user.first) + "\t" +
        std::to_string(user.second.firstMs) + "\t" +
        std::to_string(user.second.eligibleMs) + "\t" +
        std::to_string(user.second.dataMs) + "\t" +
        (user.second.hidden ? "1" : "0") + "\n";
    }
  }
  return data;
}

inline bool PresencePoller::SaveStateTo(const std::string& p_StateData, std::string& p_Error)
{
  std::string temporary = m_Dir + "/state.XXXXXX";
  std::vector<char> pathBuffer(temporary.begin(), temporary.end());
  pathBuffer.push_back('\0');
  const int fd = mkostemp(pathBuffer.data(), O_CLOEXEC);
  if (fd < 0)
  {
    p_Error = "cannot create temporary presence state: " + std::to_string(errno);
    return false;
  }
  if (fchmod(fd, 0600) != 0)
  {
    p_Error = "cannot set presence state permissions: " + std::to_string(errno);
    close(fd);
    unlink(pathBuffer.data());
    return false;
  }

  bool error = false;
  size_t offset = 0;
  while (offset < p_StateData.size())
  {
    ssize_t rv = write(fd, p_StateData.data() + offset, p_StateData.size() - offset);
    if (rv < 0)
    {
      if (errno == EINTR) continue;
      p_Error = "cannot write presence state: " + std::to_string(errno);
      error = true;
      break;
    }
    if (rv == 0)
    {
      p_Error = "cannot write presence state: short write";
      error = true;
      break;
    }
    offset += static_cast<size_t>(rv);
  }
  if (!error && fsync(fd) != 0)
  {
    p_Error = "cannot fsync presence state: " + std::to_string(errno);
    error = true;
  }
  if (close(fd) != 0 && !error)
  {
    p_Error = "cannot close presence state: " + std::to_string(errno);
    error = true;
  }

  if (!error && rename(pathBuffer.data(), (m_Dir + "/state").c_str()) != 0)
  {
    p_Error = "cannot rename presence state: " + std::to_string(errno);
    error = true;
  }

  if (error)
  {
    unlink(pathBuffer.data());
    return false;
  }

  if (!SyncDir(p_Error))
  {
    return false;
  }

  return true;
}

inline bool PresencePoller::ParseInt64(const std::string& p_Str, int64_t& p_Value)
{
  if (p_Str.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(p_Str.c_str(), &end, 10);
  // Require full consumption of the string: embedded NULs followed by
  // garbage must not pass.
  if (errno != 0 || end == p_Str.c_str() || end != p_Str.c_str() + p_Str.size())
  {
    return false;
  }
  p_Value = static_cast<int64_t>(value);
  return true;
}

inline std::string PresencePoller::EscapeJson(const std::string& p_Str)
{
  std::string out;
  out.reserve(p_Str.size() + 8);
  for (unsigned char ch : p_Str)
  {
    switch (ch)
    {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20)
        {
          static const char* hexDigits = "0123456789abcdef";
          out += "\\u00";
          out += hexDigits[(ch >> 4) & 0xf];
          out += hexDigits[ch & 0xf];
        }
        else
        {
          out += static_cast<char>(ch);
        }
        break;
    }
  }
  return out;
}

inline std::string PresencePoller::HexEncode(const std::string& p_Str)
{
  static const char* hexDigits = "0123456789abcdef";
  std::string out;
  out.reserve(p_Str.size() * 2);
  for (unsigned char ch : p_Str)
  {
    out += hexDigits[(ch >> 4) & 0xf];
    out += hexDigits[ch & 0xf];
  }
  return out;
}

inline std::string PresencePoller::EncodeStateField(const std::string& p_Str)
{
  std::string out;
  out.reserve(p_Str.size());
  for (char ch : p_Str)
  {
    switch (ch)
    {
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\\': out += "\\\\"; break;
      default:   out += ch; break;
    }
  }
  return out;
}

inline std::string PresencePoller::DecodeStateField(const std::string& p_Str)
{
  std::string out;
  out.reserve(p_Str.size());
  for (size_t i = 0; i < p_Str.size(); ++i)
  {
    if (p_Str[i] == '\\' && i + 1 < p_Str.size())
    {
      switch (p_Str[i + 1])
      {
        case 't': out += '\t'; ++i; break;
        case 'n': out += '\n'; ++i; break;
        case 'r': out += '\r'; ++i; break;
        case '\\': out += '\\'; ++i; break;
        default: out += p_Str[i]; break;
      }
    }
    else
    {
      out += p_Str[i];
    }
  }
  return out;
}
