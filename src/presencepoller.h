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
// configured observation window (hidden status) are skipped.
//
// Persistence layout under <appdir>/presence/:
//   history.<hex-profile>.jsonl  append-only samples, heartbeats and markers
//   state                        per-user polling bookkeeping (atomic rewrite)
//
// Scheduling: each user fires roughly once per interval with a random phase
// and +/-20% jitter; a per-tick dispatch budget and re-phasing of long gaps
// prevent catch-up bursts after sleep. Scheduling time is monotonic; record
// timestamps are wall clock.
//
// History integrity: appends are length-checked and rolled back on partial
// writes, an incomplete trailing line from a crash is repaired before the
// next append, and state is committed only after its history lines are
// durable. History records only genuine protocol observations plus explicit
// heartbeat re-affirmations ("hb":1) of the last known state; display-state
// guesses are never recorded.
//
// Header-only like localfeatures.h so standalone tests can build it without
// linking the full core. No core logging or protocol dependencies.

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PresencePoller
{
public:
  PresencePoller() = default;

  bool Init(const std::string& p_BaseDir, uint32_t p_RandomSeed = 0);
  bool IsInitialized() const { return m_Initialized; }
  bool HasPending() const { return !m_Pending.empty() || m_StateDirty; }
  const std::string& GetError() const { return m_Error; }

  struct UserState
  {
    int64_t firstMs = 0;   // first wall-clock time this user entered the poll set
    int64_t eligibleMs = 0; // accumulated observation time while selected (monotonic)
    int64_t dataMs = 0;    // last wall-clock time observable data was seen (0 = never)
    bool hidden = false;   // skip polling: no observable data across the window
  };

  // Scheduling tick, called with the current candidate list (most recent
  // private chats first, already truncated by the caller). p_NowMs must be
  // monotonic. Registers first-seen times, accumulates eligible observation
  // time for selected users, marks never-observed users hidden after
  // p_HiddenDetectMs of eligible time, and returns the user ids due for a
  // poll request now (bounded by a small per-tick dispatch budget).
  std::vector<std::string> Tick(int64_t p_NowMs,
                                const std::string& p_ProfileId,
                                const std::vector<std::string>& p_Candidates,
                                int64_t p_PollIntervalMs,
                                int64_t p_HiddenDetectMs);

  // Record a genuine protocol observation (service-pushed update or poll
  // response). p_NowMs is wall clock. Only candidate users are recorded;
  // identical consecutive observations are coalesced. Observable data clears
  // a previous hidden mark so the user returns to the poll set.
  void RecordStatus(int64_t p_NowMs, const std::string& p_ProfileId,
                    const std::string& p_UserId, bool p_IsOnline,
                    int64_t p_TimeSeen, bool p_Recently);

  // Re-affirm the last recorded observation with a heartbeat line
  // ("hb":1). Does nothing for users with no observation yet.
  void RecordHeartbeat(int64_t p_NowMs, const std::string& p_ProfileId,
                       const std::string& p_UserId, int64_t p_HeartbeatMs);

  // Append a "hidden" marker line to the history for auditing.
  void RecordHiddenMark(int64_t p_NowMs, const std::string& p_ProfileId,
                        const std::string& p_UserId);

  // Persist pending history lines, then any dirty state (state never commits
  // before its history lines are durable). Per-profile history failures keep
  // only that profile's lines for a later retry; partial appends are rolled
  // back to the pre-append length.
  bool Flush();

private:
  struct LastSample
  {
    int64_t tsMs = 0;
    bool online = false;
    int64_t timeSeen = -1;
    bool recently = false;
  };

  struct PendingLine
  {
    std::string profileId;
    std::string text;
  };

  static const size_t s_MaxPendingLines = 20000;
  static const size_t s_MaxDispatchPerTick = 3;
  static const int64_t s_MaxEligibleDeltaMs = 5 * 60 * 1000; // cap per-tick gap

  static std::string EscapeJson(const std::string& p_Str);
  static std::string HexEncode(const std::string& p_Str);
  static std::string EncodeStateField(const std::string& p_Str);
  static std::string DecodeStateField(const std::string& p_Str);
  static bool ParseInt64(const std::string& p_Str, int64_t& p_Value);

  bool LoadState();
  bool SaveState();
  bool AppendHistory(const std::string& p_ProfileId,
                     const std::vector<std::string>& p_Lines);
  static bool RepairIncompleteTail(int p_Fd, off_t& p_Length);
  int64_t UniformInt(int64_t p_Lo, int64_t p_Hi);
  void QueueLine(const std::string& p_ProfileId, const std::string& p_Text);

private:
  std::string m_Dir;
  std::string m_Error;
  bool m_Initialized = false;
  std::unordered_map<std::string, std::unordered_map<std::string, UserState>> m_UserStates;
  std::unordered_map<std::string, std::unordered_map<std::string, LastSample>> m_LastSamples;
  std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> m_NextPollMs;
  std::unordered_map<std::string, std::unordered_set<std::string>> m_Tracked;
  std::unordered_map<std::string, int64_t> m_LastTickMs;
  std::vector<PendingLine> m_Pending;
  bool m_StateDirty = false;
  std::mt19937_64 m_Rng;
};

inline bool PresencePoller::Init(const std::string& p_BaseDir, uint32_t p_RandomSeed)
{
  m_Dir = p_BaseDir;
  m_Error.clear();
  m_Initialized = false;

  if (mkdir(m_Dir.c_str(), 0700) != 0 && errno != EEXIST)
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
    // Unreadable or corrupt state must not be overwritten by an empty
    // reconstruction; the poller stays inert until a retry succeeds.
    return false;
  }

  m_Initialized = true;
  return true;
}

inline int64_t PresencePoller::UniformInt(int64_t p_Lo, int64_t p_Hi)
{
  if (p_Hi < p_Lo) return p_Lo;
  std::uniform_int_distribution<int64_t> dist(p_Lo, p_Hi);
  return dist(m_Rng);
}

inline std::vector<std::string> PresencePoller::Tick(int64_t p_NowMs,
                                                     const std::string& p_ProfileId,
                                                     const std::vector<std::string>& p_Candidates,
                                                     int64_t p_PollIntervalMs,
                                                     int64_t p_HiddenDetectMs)
{
  if (!m_Initialized) return std::vector<std::string>();

  if (p_PollIntervalMs < 1000) p_PollIntervalMs = 1000;

  auto& states = m_UserStates[p_ProfileId];
  bool dirty = false;

  m_Tracked[p_ProfileId].clear();
  for (const std::string& userId : p_Candidates)
  {
    m_Tracked[p_ProfileId].insert(userId);
    UserState& st = states[userId];
    if (st.firstMs == 0)
    {
      st.firstMs = p_NowMs;
      dirty = true;
    }
  }

  // Accumulate eligible observation time for selected (non-hidden) users.
  // Gaps are capped so downtime, disconnects or absence from the candidate
  // list cannot fast-forward the detection window.
  int64_t delta = 0;
  int64_t& lastTick = m_LastTickMs[p_ProfileId];
  if (lastTick == 0 || p_NowMs < lastTick)
  {
    delta = 0;
  }
  else
  {
    delta = p_NowMs - lastTick;
    if (delta > s_MaxEligibleDeltaMs) delta = s_MaxEligibleDeltaMs;
  }
  lastTick = p_NowMs;

  // Selected = non-hidden candidates. Users with no observable data for the
  // whole detection window become hidden here.
  std::vector<std::string> selected;
  selected.reserve(p_Candidates.size());
  for (const std::string& userId : p_Candidates)
  {
    UserState& st = states[userId];
    if (st.hidden) continue;
    st.eligibleMs += delta;
    if (st.dataMs == 0 && p_HiddenDetectMs > 0 && st.eligibleMs >= p_HiddenDetectMs)
    {
      st.hidden = true;
      dirty = true;
      RecordHiddenMark(p_NowMs, p_ProfileId, userId);
      continue;
    }
    selected.push_back(userId);
  }

  // Drop schedules for users no longer selected.
  auto& nextPoll = m_NextPollMs[p_ProfileId];
  std::unordered_set<std::string> selectedSet(selected.begin(), selected.end());
  for (auto it = nextPoll.begin(); it != nextPoll.end(); )
  {
    if (!selectedSet.count(it->first))
    {
      it = nextPoll.erase(it);
    }
    else
    {
      ++it;
    }
  }

  // New users start at a random offset inside the interval so the whole set
  // does not fire at once.
  for (const std::string& userId : selected)
  {
    if (!nextPoll.count(userId))
    {
      nextPoll[userId] = p_NowMs + UniformInt(0, p_PollIntervalMs - 1);
    }
  }

  // Users due now; reschedule with a +/-20% jitter around the interval.
  // Long-overdue users are re-phased instead of firing a catch-up burst, and
  // only the per-tick dispatch budget is actually fired and rescheduled; the
  // rest stay overdue and fire on later ticks.
  std::vector<std::string> due;
  const int64_t jitter = std::max<int64_t>(1000, p_PollIntervalMs / 5);
  for (const std::string& userId : selected)
  {
    int64_t& t = nextPoll[userId];
    if (t > p_NowMs) continue;
    if (t < p_NowMs - 2 * p_PollIntervalMs)
    {
      t = p_NowMs + UniformInt(0, p_PollIntervalMs - 1);
      continue;
    }
    if (due.size() >= s_MaxDispatchPerTick) break;
    due.push_back(userId);
    t = p_NowMs + p_PollIntervalMs + UniformInt(-jitter, jitter);
    if (t <= p_NowMs) t = p_NowMs + 1000;
  }

  if (dirty)
  {
    m_StateDirty = true;
  }

  return due;
}

inline void PresencePoller::QueueLine(const std::string& p_ProfileId, const std::string& p_Text)
{
  m_Pending.push_back({ p_ProfileId, p_Text });
  if (m_Pending.size() > s_MaxPendingLines)
  {
    // Bounded pending data: the oldest lines are dropped rather than growing
    // memory without limit during a persistent storage failure. The loss is
    // reported through the error string.
    const size_t drop = m_Pending.size() - s_MaxPendingLines;
    m_Pending.erase(m_Pending.begin(), m_Pending.begin() + static_cast<long>(drop));
    m_Error = "presence history overflow: dropped " + std::to_string(drop) + " oldest lines";
  }
}

inline void PresencePoller::RecordStatus(int64_t p_NowMs, const std::string& p_ProfileId,
                                         const std::string& p_UserId, bool p_IsOnline,
                                         int64_t p_TimeSeen, bool p_Recently)
{
  if (!m_Initialized) return;

  // Only users in the current poll candidate set are recorded; service
  // pushes for other chats update the UI but stay out of the history.
  const auto trackedIt = m_Tracked.find(p_ProfileId);
  if (trackedIt == m_Tracked.end() || !trackedIt->second.count(p_UserId)) return;

  UserState& st = m_UserStates[p_ProfileId][p_UserId];
  if (st.firstMs == 0)
  {
    st.firstMs = p_NowMs;
    m_StateDirty = true;
  }

  const bool hasData = p_IsOnline || (p_TimeSeen >= 0) || p_Recently;
  if (hasData)
  {
    if (st.dataMs == 0) m_StateDirty = true;
    st.dataMs = p_NowMs;
    if (st.hidden)
    {
      // Observable data after a hidden mark: the peer's status is visible
      // again, so it returns to the poll set.
      st.hidden = false;
      m_StateDirty = true;
    }
  }

  LastSample& last = m_LastSamples[p_ProfileId][p_UserId];
  if (last.tsMs != 0 &&
      last.online == p_IsOnline &&
      last.timeSeen == p_TimeSeen &&
      last.recently == p_Recently)
  {
    return; // identical consecutive observation
  }

  last = { p_NowMs, p_IsOnline, p_TimeSeen, p_Recently };

  std::string line = "{\"ts\":" + std::to_string(p_NowMs) +
    ",\"u\":\"" + EscapeJson(p_UserId) + "\"" +
    ",\"on\":" + (p_IsOnline ? "1" : "0") +
    ",\"seen\":" + std::to_string(p_TimeSeen) +
    ",\"recent\":" + (p_Recently ? "1" : "0") + "}";
  QueueLine(p_ProfileId, line);
}

inline void PresencePoller::RecordHeartbeat(int64_t p_NowMs, const std::string& p_ProfileId,
                                            const std::string& p_UserId, int64_t p_HeartbeatMs)
{
  if (!m_Initialized) return;
  if (p_HeartbeatMs < 1000) p_HeartbeatMs = 1000;

  LastSample& last = m_LastSamples[p_ProfileId][p_UserId];
  if (last.tsMs == 0)
  {
    return; // nothing observed yet; never fabricate a sample
  }
  if (p_NowMs - last.tsMs < p_HeartbeatMs)
  {
    return;
  }
  last.tsMs = p_NowMs;

  std::string line = "{\"ts\":" + std::to_string(p_NowMs) +
    ",\"u\":\"" + EscapeJson(p_UserId) + "\"" +
    ",\"on\":" + (last.online ? "1" : "0") +
    ",\"seen\":" + std::to_string(last.timeSeen) +
    ",\"recent\":" + (last.recently ? "1" : "0") +
    ",\"hb\":1}";
  QueueLine(p_ProfileId, line);
}

inline void PresencePoller::RecordHiddenMark(int64_t p_NowMs, const std::string& p_ProfileId,
                                             const std::string& p_UserId)
{
  std::string line = "{\"ts\":" + std::to_string(p_NowMs) +
    ",\"u\":\"" + EscapeJson(p_UserId) + "\",\"mark\":\"hidden\"}";
  QueueLine(p_ProfileId, line);
}

inline bool PresencePoller::Flush()
{
  if (!m_Initialized) return false;

  bool ok = true;

  if (!m_Pending.empty())
  {
    // Group lines per profile, preserving line order, so each history file
    // is opened and appended exactly once per flush.
    std::map<std::string, std::vector<std::string>> batches;
    for (const PendingLine& line : m_Pending)
    {
      batches[line.profileId].push_back(line.text);
    }

    std::vector<PendingLine> remaining;
    for (const auto& batch : batches)
    {
      if (AppendHistory(batch.first, batch.second))
      {
        // This profile's lines are durable; the failed ones are retried
        // later and never re-appended in full.
        continue;
      }
      ok = false;
      for (const std::string& text : batch.second)
      {
        remaining.push_back({ batch.first, text });
      }
    }
    m_Pending.swap(remaining);
  }

  if (ok && m_Pending.empty() && m_StateDirty)
  {
    // State commits only after all history lines are durable, so a hidden
    // decision can never be persisted without its marker line.
    if (!SaveState())
    {
      ok = false;
    }
  }

  return ok;
}

inline bool PresencePoller::AppendHistory(const std::string& p_ProfileId,
                                          const std::vector<std::string>& p_Lines)
{
  const std::string path = m_Dir + "/history." + HexEncode(p_ProfileId) + ".jsonl";

  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
  {
    // Recreate the directory in case it was removed while running.
    if (mkdir(m_Dir.c_str(), 0700) == 0 || errno == EEXIST)
    {
      fd = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
  }
  if (fd < 0)
  {
    m_Error = "cannot open presence history " + path + ": " + std::to_string(errno);
    return false;
  }

  struct stat fileStat;
  if (fstat(fd, &fileStat) != 0 || !S_ISREG(fileStat.st_mode))
  {
    m_Error = "presence history is not a regular file: " + path;
    close(fd);
    return false;
  }
  if (fchmod(fd, 0600) != 0)
  {
    m_Error = "cannot set presence history permissions on " + path + ": " + std::to_string(errno);
    close(fd);
    return false;
  }

  const bool created = (fileStat.st_size == 0);

  off_t preLen = lseek(fd, 0, SEEK_END);
  if (preLen < 0 || !RepairIncompleteTail(fd, preLen))
  {
    m_Error = "cannot prepare presence history " + path + ": " + std::to_string(errno);
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
    // leave a malformed trailing fragment.
    if (ftruncate(fd, preLen) != 0)
    {
      m_Error = "cannot roll back partial presence history append on " + path +
        ": " + std::to_string(errno);
    }
    else
    {
      m_Error = "cannot write presence history " + path + ": " + std::to_string(errno);
    }
    close(fd);
    return false;
  }

  if (close(fd) != 0)
  {
    // Rare deferred close failure: the data is already fsynced, so retrying
    // could duplicate lines. Report and treat the batch as written.
    m_Error = "close failed on presence history " + path + ": " + std::to_string(errno);
    return true;
  }

  if (created)
  {
    // Newly created history files need a directory sync to persist the
    // directory entry itself.
    const int dirFd = open(m_Dir.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (dirFd >= 0)
    {
      fsync(dirFd);
      close(dirFd);
    }
  }

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
  // the last newline so the next append starts on a fresh record.
  const off_t scanStart = (p_Length > 65536) ? (p_Length - 65536) : 0;
  const size_t scanSize = static_cast<size_t>(p_Length - scanStart);
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

  off_t cutOffset = -1;
  for (off_t i = static_cast<off_t>(readOffset) - 1; i >= 0; --i)
  {
    if (buffer[static_cast<size_t>(i)] == '\n')
    {
      cutOffset = scanStart + i + 1;
      break;
    }
  }
  if (cutOffset < 0)
  {
    // No complete line exists; drop everything.
    cutOffset = 0;
  }

  if (ftruncate(p_Fd, cutOffset) != 0) return false;
  p_Length = cutOffset;
  return true;
}

inline bool PresencePoller::LoadState()
{
  const std::string path = m_Dir + "/state";
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
  {
    if (errno == ENOENT) return true; // fresh install: empty state
    m_Error = "cannot open presence state " + path + ": " + std::to_string(errno);
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
  // whole file has been read successfully.
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
    if (firstMs <= 0) continue;
    const bool hidden = (fields[5] == "1");
    if (fields[5] != "0" && fields[5] != "1") continue;

    UserState st;
    st.firstMs = firstMs;
    st.eligibleMs = eligibleMs;
    st.dataMs = dataMs;
    st.hidden = hidden;

    parsed[DecodeStateField(fields[0])][DecodeStateField(fields[1])] = st;
  }

  m_UserStates.swap(parsed);
  return true;
}

inline bool PresencePoller::SaveState()
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

  std::string temporary = m_Dir + "/state.XXXXXX";
  std::vector<char> pathBuffer(temporary.begin(), temporary.end());
  pathBuffer.push_back('\0');
  const int fd = mkostemp(pathBuffer.data(), O_CLOEXEC);
  if (fd < 0)
  {
    m_Error = "cannot create temporary presence state: " + std::to_string(errno);
    return false;
  }
  if (fchmod(fd, 0600) != 0)
  {
    m_Error = "cannot set presence state permissions: " + std::to_string(errno);
    close(fd);
    unlink(pathBuffer.data());
    return false;
  }

  bool error = false;
  size_t offset = 0;
  while (offset < data.size())
  {
    ssize_t rv = write(fd, data.data() + offset, data.size() - offset);
    if (rv < 0)
    {
      if (errno == EINTR) continue;
      m_Error = "cannot write presence state: " + std::to_string(errno);
      error = true;
      break;
    }
    if (rv == 0)
    {
      m_Error = "cannot write presence state: short write";
      error = true;
      break;
    }
    offset += static_cast<size_t>(rv);
  }
  if (!error && fsync(fd) != 0)
  {
    m_Error = "cannot fsync presence state: " + std::to_string(errno);
    error = true;
  }
  if (close(fd) != 0 && !error)
  {
    m_Error = "cannot close presence state: " + std::to_string(errno);
    error = true;
  }

  if (!error && rename(pathBuffer.data(), (m_Dir + "/state").c_str()) != 0)
  {
    m_Error = "cannot rename presence state: " + std::to_string(errno);
    error = true;
  }

  if (error)
  {
    unlink(pathBuffer.data());
    return false;
  }

  const int dirFd = open(m_Dir.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (dirFd >= 0)
  {
    if (fsync(dirFd) != 0)
    {
      m_Error = "cannot fsync presence dir: " + std::to_string(errno);
      close(dirFd);
      return false; // keep dirty so the save is retried
    }
    close(dirFd);
  }

  m_StateDirty = false;
  return true;
}

inline bool PresencePoller::ParseInt64(const std::string& p_Str, int64_t& p_Value)
{
  if (p_Str.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(p_Str.c_str(), &end, 10);
  if (errno != 0 || end == p_Str.c_str() || *end != '\0') return false;
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
