// test-presence-poller.cpp
//
// Standalone regression test for PresencePoller (header-only). Exercises
// recording/coalescing/heartbeats, gating, randomized scheduling with the
// dispatch budget and re-phasing, eligible-time hidden detection with
// distinct wall/steady clock domains, hidden recovery, persistence across
// reloads, flush-failure rollback (no duplicated lines), blocked-profile
// rollback retry, crash-tail repair including fragments larger than one
// scan chunk, corrupt state handling and init failure.
//
// Uses a private temp directory; no real protocols, accounts or message data.
// Run with an optional base directory argument to keep the generated files
// for external validation (the shell wrapper validates the JSONL with
// python3 and removes the directory afterwards).

#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>

#include "../src/presencepoller.h"

static const int64_t kBase = 1000000000;      // synthetic steady base
static const int64_t kWallOffset = 1500000000000LL; // wall = steady + offset

static std::string ReadFile(const std::string& p_Path)
{
  std::ifstream file(p_Path);
  assert(file.good());
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

static size_t CountLines(const std::string& p_Text)
{
  size_t count = 0;
  for (char ch : p_Text)
  {
    if (ch == '\n') ++count;
  }
  return count;
}

static size_t CountOccurrences(const std::string& p_Haystack, const std::string& p_Needle)
{
  size_t count = 0;
  size_t pos = 0;
  while ((pos = p_Haystack.find(p_Needle, pos)) != std::string::npos)
  {
    ++count;
    pos += p_Needle.size();
  }
  return count;
}

static std::string Hex(const std::string& p_Str)
{
  static const char* hexDigits = "0123456789abcdef";
  std::string out;
  for (unsigned char ch : p_Str)
  {
    out += hexDigits[(ch >> 4) & 0xf];
    out += hexDigits[ch & 0xf];
  }
  return out;
}

static bool Contains(const std::vector<std::string>& p_Vec, const std::string& p_Value)
{
  return std::find(p_Vec.begin(), p_Vec.end(), p_Value) != p_Vec.end();
}

int main(int argc, char* argv[])
{
  std::string dir;
  bool ownDir = false;
  if (argc >= 2)
  {
    dir = argv[1];
    mkdir(dir.c_str(), 0700);
  }
  else
  {
    char tempDirTemplate[] = "/tmp/nchat-presence-poller.XXXXXX";
    char* tempDir = mkdtemp(tempDirTemplate);
    assert(tempDir != nullptr);
    dir = tempDir;
    ownDir = true;
  }

  const std::string profile("Telegram_test");
  const std::string statePath = dir + "/state";

  {
    // Recording, coalescing, heartbeat re-affirmation and gating. Distinct
    // wall and steady clocks: stored ts values must be wall-clock.
    PresencePoller poller;
    assert(poller.Init(dir, 12345));
    assert(poller.IsInitialized());

    const std::string specialUser = "u\"x\\\ty";
    std::vector<std::string> candidates = { "u1", specialUser };
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, candidates, 60000, 120000, true).empty());

    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, profile, "u1", false, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());

    const std::string historyPath = dir + "/history." + Hex(profile) + ".jsonl";
    std::string history = ReadFile(historyPath);
    assert(CountLines(history) == 1);
    assert(history.find("\"ts\":1501000001000") != std::string::npos); // wall clock
    assert(history.find("\"u\":\"u1\"") != std::string::npos);
    assert(history.find("\"on\":0") != std::string::npos);
    assert(history.find("\"seen\":-1") != std::string::npos);
    assert(history.find("\"recent\":0") != std::string::npos);
    assert(history.find("\"hb\"") == std::string::npos);

    // Identical consecutive observation is coalesced.
    poller.RecordStatus(kBase + 2000, kBase + kWallOffset + 2000, profile, "u1", false, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());
    assert(CountLines(ReadFile(historyPath)) == 1);

    // A state change is always recorded.
    poller.RecordStatus(kBase + 3000, kBase + kWallOffset + 3000, profile, "u1", true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());
    assert(CountLines(ReadFile(historyPath)) == 2);

    // Heartbeat: no-op inside the window, re-affirmation after it. The gate
    // uses steady time, the line uses wall time.
    poller.RecordHeartbeat(kBase + 4000, kBase + kWallOffset + 4000, profile, "u1", 600000);
    assert(poller.Flush());
    assert(poller.Drain());
    assert(CountLines(ReadFile(historyPath)) == 2);
    poller.RecordHeartbeat(kBase + 700000, kBase + kWallOffset + 700000, profile, "u1", 600000);
    assert(poller.Flush());
    assert(poller.Drain());
    history = ReadFile(historyPath);
    assert(CountLines(history) == 3);
    assert(history.find("\"ts\":1501000700000") != std::string::npos); // wall clock
    assert(history.find("\"hb\":1") != std::string::npos);

    // Users outside the poll set are never recorded, and heartbeats for
    // never-observed users never fabricate samples.
    poller.RecordStatus(kBase + 800000, kBase + kWallOffset + 800000, profile, "uOut", true, -1, false);
    poller.RecordHeartbeat(kBase + 800000, kBase + kWallOffset + 800000, profile, "uOut", 600000);
    assert(poller.Flush());
    assert(poller.Drain());
    assert(CountLines(ReadFile(historyPath)) == 3);

    // JSON escaping of special characters in user ids.
    poller.RecordStatus(kBase + 900000, kBase + kWallOffset + 900000, profile, specialUser, false, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());
    history = ReadFile(historyPath);
    assert(history.find("\\\"x\\\\\\ty") != std::string::npos);

    // A user that drops out of the candidate list stops being recorded.
    std::vector<std::string> onlyU1 = { "u1" };
    poller.Tick(kBase + 1000000, kBase + kWallOffset + 1000000, profile, onlyU1, 60000, 120000, true);
    poller.RecordStatus(kBase + 1001000, kBase + kWallOffset + 1001000, profile, specialUser, true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());
    history = ReadFile(historyPath);
    assert(CountOccurrences(history, "\"u\":\"u\\\"x\\\\\\ty\"") == 1);

    // Hidden markers are queued by Tick and use wall-clock timestamps.
    std::vector<std::string> hiddenCandidates = { "h1" };
    for (int64_t t = 1001000; t <= 2100000; t += 1000)
    {
      poller.Tick(kBase + t, kBase + kWallOffset + t, profile, hiddenCandidates, 60000, 1000000, true);
    }
    assert(poller.Flush());
    assert(poller.Drain());
    history = ReadFile(historyPath);
    const size_t markPos = history.find("\"mark\":\"hidden\"");
    assert(markPos != std::string::npos);
    const size_t markLineStart = history.rfind('\n', markPos) + 1;
    const std::string markerLine =
      history.substr(markLineStart, history.find('\n', markPos) - markLineStart);
    assert(markerLine.find("\"ts\":1501") != std::string::npos); // wall clock, not steady
  }

  {
    // Scheduling, eligible-time hidden detection, recovery and persistence.
    PresencePoller poller;
    assert(poller.Init(dir, 999));

    std::vector<std::string> candidates = { "a", "b", "c", "d" };
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, candidates, 60000, 120000, true).empty());
    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, profile, "b", true, -1, false);

    // Spread across the interval, at least 48s between consecutive fires.
    std::unordered_map<std::string, int64_t> lastFire;
    for (int64_t t = 1000; t <= 80000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 120000, true);
      for (const std::string& u : due)
      {
        const auto it = lastFire.find(u);
        if (it == lastFire.end())
        {
          lastFire[u] = kBase + t;
        }
        else
        {
          assert(kBase + t - it->second >= 47000);
          it->second = kBase + t;
        }
      }
    }
    assert(lastFire.size() == 4);

    // After 120s of eligible time, every never-observed user is hidden:
    // "a", "c" and "d" (only "b" ever showed data). "b" keeps polling.
    bool bSeen = false;
    for (int64_t t = 47000; t <= 121000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 120000, true);
      for (const std::string& u : due)
      {
        if (t >= 120000)
        {
          assert(u != "a" && u != "c" && u != "d");
        }
        if (u == "b") bSeen = true;
      }
    }
    assert(bSeen);

    // Observable data for a hidden user returns it to the poll set.
    poller.RecordStatus(kBase + 150000, kBase + kWallOffset + 150000, profile, "c", true, -1, false);
    bool cSeen = false;
    for (int64_t t = 151000; t <= 230000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 120000, true);
      for (const std::string& u : due)
      {
        assert(u != "a" && u != "d");
        if (u == "c") cSeen = true;
      }
    }
    assert(cSeen);
    assert(poller.Flush(true)); // force a state checkpoint with eligible/data times
    assert(poller.Drain());

    // The state file persisted hidden=1 for a/d with no data, and b/c
    // un-hidden with observable data.
    const std::string state = ReadFile(statePath);
    assert(state.find(profile + "\tb\t") != std::string::npos);
    for (const std::string u : { "a", "d" })
    {
      const size_t pos = state.find(profile + "\t" + u + "\t");
      assert(pos != std::string::npos);
      assert(state.find("\t0\t1\n", pos) != std::string::npos); // data=0, hidden=1
    }
    for (const std::string u : { "b", "c" })
    {
      const size_t pos = state.find(profile + "\t" + u + "\t");
      assert(pos != std::string::npos);
      const std::string line = state.substr(pos, state.find('\n', pos) - pos);
      const size_t lastTab = line.rfind('\t');
      assert(lastTab != std::string::npos);
      assert(line.substr(lastTab + 1) == "0"); // un-hidden
    }

    // Reload from disk: hidden marks survive, and "c" must fire even after
    // the detection window (its persisted observation proves the round trip,
    // not just a fresh registration).
    PresencePoller reloaded;
    assert(reloaded.Init(dir, 999));
    bool cSeenAfterWindow = false;
    for (int64_t t = 231000; t <= 400000; t += 1000)
    {
      std::vector<std::string> due =
        reloaded.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 120000, true);
      for (const std::string& u : due)
      {
        assert(u != "a" && u != "d");
        if (u == "c" && t > 231000 + 120000)
        {
          cSeenAfterWindow = true;
        }
      }
    }
    assert(cSeenAfterWindow);
    // Eligible time persisted: "c" accumulated through the deadline in the
    // first session and must not be hidden by the reloaded poller (dataMs
    // would prevent hiding anyway; the positive check above proves it).
  }

  {
    // Eligible time accrues only while the account can observe, and only
    // for users already selected in the previous tick.
    PresencePoller poller;
    assert(poller.Init(dir, 51));
    std::vector<std::string> candidates = { "e", "f" };
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, candidates, 60000, 600000, false).empty());
    // Long unobservable gap: polling continues but no eligible time accrues.
    for (int64_t t = 1000; t <= 300000; t += 1000)
    {
      poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 600000, false);
    }
    // Observable: eligible time accrues 1000 ms per tick, so the 600s window
    // is reached at t = 301000 + 599000 = 900000. Until then e/f poll
    // normally; afterwards they must be hidden.
    for (int64_t t = 301000; t <= 1000000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 600000, true);
      for (const std::string& u : due)
      {
        if (t >= 901000)
        {
          assert(u != "e" && u != "f");
        }
      }
    }
    // Newcomer "g" joins after the window would have elapsed: it must not
    // inherit e/f's accrued time. If it had, it would be hidden instantly
    // and never fire; instead it must appear in the poll output.
    std::vector<std::string> withNewcomer = { "e", "f", "g" };
    assert(!Contains(poller.Tick(kBase + 1001000, kBase + kWallOffset + 1001000, profile,
                                 withNewcomer, 60000, 600000, true), "g"));
    bool gSeen = false;
    for (int64_t t = 1002000; t <= 1300000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, withNewcomer, 60000, 600000, true);
      for (const std::string& u : due)
      {
        assert(u != "e" && u != "f");
        if (u == "g") gSeen = true;
      }
    }
    assert(gSeen);
    assert(poller.Flush());
    assert(poller.Drain());
  }

  {
    // Dispatch budget and long-gap re-phasing.
    std::vector<std::string> many;
    for (int i = 0; i < 30; ++i)
    {
      many.push_back("m" + std::to_string(i));
    }

    PresencePoller poller;
    assert(poller.Init(dir, 42));
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, many, 60000, 0, true).empty());
    // One interval later everyone is due, but the per-tick budget caps it.
    std::vector<std::string> due =
      poller.Tick(kBase + 60000, kBase + kWallOffset + 60000, profile, many, 60000, 0, true);
    assert(due.size() == 3);
    due = poller.Tick(kBase + 61000, kBase + kWallOffset + 61000, profile, many, 60000, 0, true);
    assert(due.size() == 3);

    // A long gap re-phases overdue users instead of firing a burst.
    PresencePoller paused;
    assert(paused.Init(dir, 43));
    std::vector<std::string> five = { "p0", "p1", "p2", "p3", "p4" };
    assert(paused.Tick(kBase, kBase + kWallOffset, profile, five, 60000, 0, true).empty());
    due = paused.Tick(kBase + 10 * 60000 + 1000, kBase + kWallOffset + 10 * 60000 + 1000,
                      profile, five, 60000, 0, true);
    assert(due.empty()); // re-phased, nothing fired at the wake-up tick
    std::unordered_set<std::string> fired;
    for (int64_t t = 602000; t <= 700000; t += 1000)
    {
      due = paused.Tick(kBase + t, kBase + kWallOffset + t, profile, five, 60000, 0, true);
      assert(due.size() <= 3);
      for (const std::string& u : due)
      {
        fired.insert(u);
      }
    }
    assert(fired.size() == 5);
  }

  {
    // Flush failure rollback: a failing profile keeps only its own lines,
    // healthy profiles commit once, and retries never duplicate.
    const std::string pa = "Telegram_a";
    const std::string pb = "Telegram_b";
    PresencePoller poller;
    assert(poller.Init(dir, 7));

    assert(poller.Tick(kBase, kBase + kWallOffset, pa, { "u1" }, 60000, 0, true).empty());
    assert(poller.Tick(kBase, kBase + kWallOffset, pb, { "v1" }, 60000, 0, true).empty());
    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, pa, "u1", true, -1, false);
    poller.RecordStatus(kBase + 2000, kBase + kWallOffset + 2000, pb, "v1", true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());

    const std::string pathA = dir + "/history." + Hex(pa) + ".jsonl";
    const std::string pathB = dir + "/history." + Hex(pb) + ".jsonl";
    assert(CountLines(ReadFile(pathA)) == 1);
    assert(CountLines(ReadFile(pathB)) == 1);

    // Make profile A's file unwritable, then record a change for both.
    assert(chmod(pathA.c_str(), 0400) == 0);
    poller.RecordStatus(kBase + 3000, kBase + kWallOffset + 3000, pa, "u1", false, 1234567890000LL, false);
    poller.RecordStatus(kBase + 3000, kBase + kWallOffset + 3000, pb, "v1", false, 1234567890000LL, false);
    assert(poller.Flush());
    std::string workerError;
    bool gotError = false;
    for (int i = 0; i < 200 && !gotError; ++i)
    {
      gotError = poller.TakeWorkerError(workerError);
      if (!gotError) usleep(10000);
    }
    assert(gotError); // the worker reported A's failure

    // B committed; A kept its single line.
    assert(CountLines(ReadFile(pathB)) == 2);
    assert(CountLines(ReadFile(pathA)) == 1);

    // Restore and retry: A gets exactly one copy of the pending line.
    assert(chmod(pathA.c_str(), 0600) == 0);
    assert(poller.Flush());
    assert(poller.Drain()); // worker retries after its backoff
    const std::string aHistory = ReadFile(pathA);
    assert(CountLines(aHistory) == 2);
    assert(CountOccurrences(aHistory, "\"seen\":1234567890000") == 1);
  }

  {
    // Partial-append rollback under a file-size limit: a write that fails
    // mid-batch must not leave partial records, and the retained batch must
    // not duplicate on retry. Then the same with a failing rollback: the
    // profile is blocked until the rollback succeeds, still without
    // duplicates.
    const std::string pc = "Telegram_c";
    const std::string pathC = dir + "/history." + Hex(pc) + ".jsonl";
    PresencePoller poller;
    assert(poller.Init(dir, 13));
    assert(poller.Tick(kBase, kBase + kWallOffset, pc, { "u1" }, 60000, 0, true).empty());
    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, pc, "u1", true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());

    const off_t preLen = static_cast<off_t>(ReadFile(pathC).size());
    struct rlimit savedLimit;
    assert(getrlimit(RLIMIT_FSIZE, &savedLimit) == 0);
    struct rlimit smallLimit = savedLimit;
    smallLimit.rlim_cur = static_cast<rlim_t>(preLen + 10);
    assert(setrlimit(RLIMIT_FSIZE, &smallLimit) == 0);
    signal(SIGXFSZ, SIG_IGN);

    // New state change produces a line longer than the remaining budget.
    poller.RecordStatus(kBase + 2000, kBase + kWallOffset + 2000, pc, "u1", false, 1234567890000LL, false);
    assert(poller.Flush());
    std::string workerError;
    bool gotError = false;
    for (int i = 0; i < 200 && !gotError; ++i)
    {
      gotError = poller.TakeWorkerError(workerError);
      if (!gotError) usleep(10000);
    }
    assert(gotError);
    // Rollback succeeded: the file still ends cleanly at the pre-append
    // length.
    assert(static_cast<off_t>(ReadFile(pathC).size()) == preLen);

    // Retry with the limit lifted: exactly one copy lands.
    assert(setrlimit(RLIMIT_FSIZE, &savedLimit) == 0);
    assert(poller.Flush());
    assert(poller.Drain()); // worker retries after its backoff
    std::string history = ReadFile(pathC);
    assert(CountLines(history) == 2);
    assert(CountOccurrences(history, "\"seen\":1234567890000") == 1);

    // Now make the rollback itself fail: the profile is blocked and no
    // duplicates may appear once the rollback succeeds.
    assert(getrlimit(RLIMIT_FSIZE, &savedLimit) == 0);
    smallLimit.rlim_cur = static_cast<rlim_t>(ReadFile(pathC).size() + 10);
    assert(setrlimit(RLIMIT_FSIZE, &smallLimit) == 0);
    PresencePoller::TestFailTruncate() = true;
    poller.RecordStatus(kBase + 3000, kBase + kWallOffset + 3000, pc, "u1", true, -1, false);
    assert(poller.Flush());
    gotError = false;
    for (int i = 0; i < 200 && !gotError; ++i)
    {
      gotError = poller.TakeWorkerError(workerError);
      if (!gotError) usleep(10000);
    }
    assert(gotError); // write failed AND rollback failed -> blocked
    PresencePoller::TestFailTruncate() = false;
    assert(setrlimit(RLIMIT_FSIZE, &savedLimit) == 0);
    // The blocked batch is still pending; the worker restores the rollback
    // after its backoff and only then re-appends the batch.
    assert(poller.Drain());
    history = ReadFile(pathC);
    assert(CountOccurrences(history, "\"on\":1") == 2); // original + retried once
  }

  {
    // Crash-tail repair, including a torn fragment larger than one 64 KiB
    // scan chunk: complete lines beyond the chunk must survive.
    const std::string pd = "Telegram_d";
    const std::string pathD = dir + "/history." + Hex(pd) + ".jsonl";
    {
      std::ofstream out(pathD, std::ios::binary);
      std::string bigUserId(200000, 'a'); // > 64 KiB torn fragment
      out << "{\"ts\":999,\"u\":\"" << bigUserId << "\",\"on\":0,\"seen\":-1,\"recent\":0}\n";
      out << "{\"ts\":100"; // torn line, no newline
    }
    const size_t completeLineSize = ReadFile(pathD).find('\n') + 1;

    PresencePoller poller;
    assert(poller.Init(dir, 11));
    assert(poller.Tick(kBase, kBase + kWallOffset, pd, { "u1" }, 60000, 0, true).empty());
    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, pd, "u1", true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());

    const std::string repaired = ReadFile(pathD);
    assert(repaired.find("{\"ts\":999,") == 0); // complete line preserved
    assert(CountLines(repaired) == 2);
    assert(repaired.back() == '\n');
    assert(repaired.find("100{\"ts\":") == std::string::npos); // torn fragment removed
    assert(static_cast<size_t>(repaired.find('\n') + 1) == completeLineSize);
  }

  {
    // Corrupt state lines are skipped; negative/domain-invalid values are
    // rejected; an unreadable state file fails initialization and keeps the
    // poller inert instead of overwriting it.
    std::string corruptDir = dir + "/corrupt";
    mkdir(corruptDir.c_str(), 0700);
    {
      std::ofstream out(corruptDir + "/state", std::ios::binary);
      out << "garbage line\n";
      out << "Telegram_x\tu\tabc\t1\t2\t3\n";        // non-numeric first
      out << "Telegram_x\tu2\t123\t456\t789\t7\n";   // invalid hidden value
      out << "Telegram_x\tu3\t123\t456\t789\t1\n";   // valid hidden=1
      out << "Telegram_x\tu4\t123\t-5\t789\t0\n";    // negative eligible
      out << "Telegram_x\tu5\t123\t456\t-9\t0\n";    // negative data
      out << "Telegram_x\tu6\t123\t45\t789\t0\n";    // trailing field count
      out << "Telegram_x\tu7\t123\t456\t789\t0\tx\n"; // too many fields
      out << "Telegram_x\tu8\t123\t456\t789\x00garbage\t0\n"; // embedded NUL
      out << "Telegram_x\tu3\textra\n";              // wrong field count
    }

    PresencePoller poller;
    assert(poller.Init(corruptDir, 5));
    assert(poller.IsInitialized());
    // u3 was loaded as hidden; u2/u4..u8 are fresh and polled normally.
    bool u2Seen = false;
    for (int64_t t = 6000; t <= 200000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, "Telegram_x",
                    { "u2", "u3", "u4", "u5", "u6", "u7", "u8" }, 60000, 0, true);
      assert(!Contains(due, "u3"));
      if (Contains(due, "u2")) u2Seen = true;
    }
    assert(u2Seen);

    // An unreadable state file fails initialization and keeps the poller
    // inert instead of overwriting the file with an empty reconstruction.
    assert(chmod((corruptDir + "/state").c_str(), 0000) == 0);
    PresencePoller blocked;
    assert(!blocked.Init(corruptDir, 5));
    assert(!blocked.IsInitialized());
    assert(blocked.Tick(kBase, kBase + kWallOffset, "Telegram_x", { "u2" }, 60000, 0, true).empty());
    blocked.RecordStatus(kBase, kBase + kWallOffset, "Telegram_x", "u2", true, -1, false);
    assert(!blocked.Flush());
    assert(chmod((corruptDir + "/state").c_str(), 0600) == 0);
    PresencePoller retried;
    assert(retried.Init(corruptDir, 5));
    assert(retried.IsInitialized());
  }

  {
    // Transient state-save failure must not wipe the state file: the worker
    // keeps the serialized snapshot until the save succeeds (a previous
    // version swapped it out and later wrote an empty state file).
    std::string transientDir = dir + "/transient";
    mkdir(transientDir.c_str(), 0700);
    const std::string transientState = transientDir + "/state";
    {
      PresencePoller poller;
      assert(poller.Init(transientDir, 21));
      assert(poller.Tick(kBase, kBase + kWallOffset, "Telegram_z", { "z1" }, 60000, 0, true).empty());
      assert(poller.Flush(true));
      assert(poller.Drain());
      assert(ReadFile(transientState).find("Telegram_z\tz1\t") != std::string::npos);

      // Make the state save fail (no mkostemp possible), add a new user so a
      // fresh state snapshot is forced, and confirm the worker reports it.
      assert(chmod(transientDir.c_str(), 0500) == 0);
      poller.Tick(kBase + 1000, kBase + kWallOffset + 1000, "Telegram_z", { "z1", "z2" }, 60000, 0, true);
      assert(poller.Flush(true));
      std::string workerError;
      bool gotError = false;
      for (int i = 0; i < 200 && !gotError; ++i)
      {
        gotError = poller.TakeWorkerError(workerError);
        if (!gotError) usleep(10000);
      }
      assert(gotError);

      // Restore the directory: the retry must write the FULL snapshot, not
      // an empty file.
      assert(chmod(transientDir.c_str(), 0700) == 0);
      assert(poller.Drain());
      const std::string state = ReadFile(transientState);
      assert(!state.empty());
      assert(state.find("Telegram_z\tz1\t") != std::string::npos);
      assert(state.find("Telegram_z\tz2\t") != std::string::npos);
    }

    // Shutdown must not hang on a persistently failing device: it exits
    // through the failure-tolerant worker path and reports the loss.
    {
      PresencePoller poller;
      assert(poller.Init(transientDir, 22));
      poller.Tick(kBase, kBase + kWallOffset, "Telegram_z", { "z3" }, 60000, 0, true);
      assert(poller.Flush(true));
      assert(poller.Drain());

      assert(chmod(transientDir.c_str(), 0500) == 0);
      poller.Tick(kBase + 1000, kBase + kWallOffset + 1000, "Telegram_z", { "z3", "z4" }, 60000, 0, true);
      assert(poller.Flush(true));
      std::string workerError;
      bool gotError = false;
      for (int i = 0; i < 200 && !gotError; ++i)
      {
        gotError = poller.TakeWorkerError(workerError);
        if (!gotError) usleep(10000);
      }
      assert(gotError);
      assert(chmod(transientDir.c_str(), 0700) == 0);
      poller.Shutdown(); // must return promptly even though a save had failed
      const std::string after = ReadFile(transientState);
      assert(!after.empty()); // never wiped by the transient failure
      assert(after.find("Telegram_z\tz3\t") != std::string::npos);
    }
  }

  {
    // State fields with tabs/newlines in both profile and user ids survive
    // a reload, and polling continues beyond the detection window (proving
    // the observation really was restored, not just re-registered).
    const std::string trickyProfile = "Telegram_\tx";
    const std::string trickyUser = "x\ty\nz";
    PresencePoller poller;
    assert(poller.Init(dir, 7));
    assert(poller.Tick(kBase, kBase + kWallOffset, trickyProfile, { trickyUser }, 60000, 120000, true).empty());
    poller.RecordStatus(kBase + 5000, kBase + kWallOffset + 5000, trickyProfile, trickyUser, true, -1, false);
    assert(poller.Flush(true));
    assert(poller.Drain());

    PresencePoller reloaded;
    assert(reloaded.Init(dir, 7));
    bool seenAfterWindow = false;
    for (int64_t t = 1000; t <= 300000; t += 1000)
    {
      std::vector<std::string> due =
        reloaded.Tick(kBase + t, kBase + kWallOffset + t, trickyProfile, { trickyUser }, 60000, 120000, true);
      if (Contains(due, trickyUser) && t > 120000)
      {
        seenAfterWindow = true;
      }
    }
    assert(seenAfterWindow);
  }

  if (ownDir)
  {
    const std::string cleanup = "rm -rf " + dir;
    assert(std::system(cleanup.c_str()) == 0);
  }

  std::printf("test-presence-poller: all checks passed\n");
  return 0;
}
