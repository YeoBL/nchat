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
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, candidates, 60000, 60000, 0, 120000, true).empty());

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
    poller.Tick(kBase + 1000000, kBase + kWallOffset + 1000000, profile, onlyU1, 60000, 60000, 0, 120000, true);
    poller.RecordStatus(kBase + 1001000, kBase + kWallOffset + 1001000, profile, specialUser, true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());
    history = ReadFile(historyPath);
    assert(CountOccurrences(history, "\"u\":\"u\\\"x\\\\\\ty\"") == 1);

    // Hidden markers are queued by Tick and use wall-clock timestamps.
    std::vector<std::string> hiddenCandidates = { "h1" };
    for (int64_t t = 1001000; t <= 2100000; t += 1000)
    {
      poller.Tick(kBase + t, kBase + kWallOffset + t, profile, hiddenCandidates, 60000, 60000, 0, 1000000, true);
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
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, candidates, 60000, 60000, 0, 120000, true).empty());
    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, profile, "b", true, -1, false);

    // One continuous timeline: spread across the interval, at least 48s
    // between consecutive fires, and after exactly 120s of eligible time the
    // never-observed users are hidden ("a"/"c"/"d"; "b" showed data and keeps
    // polling). Eligible accrues 1000 ms per tick from t=1000, so the window
    // is reached at t=120000.
    std::unordered_map<std::string, int64_t> lastFire;
    bool bSeen = false;
    for (int64_t t = 1000; t <= 121000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 60000, 0, 120000, true);
      for (const std::string& u : due)
      {
        if (t >= 120000)
        {
          assert(u != "a" && u != "c" && u != "d");
        }
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
        if (u == "b") bSeen = true;
      }
    }
    assert(lastFire.size() == 4);
    assert(bSeen);

    // Observable data for a hidden user returns it to the poll set.
    poller.RecordStatus(kBase + 150000, kBase + kWallOffset + 150000, profile, "c", true, -1, false);
    bool cSeen = false;
    for (int64_t t = 151000; t <= 230000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 60000, 0, 120000, true);
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
        reloaded.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 60000, 0, 120000, true);
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
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, candidates, 60000, 60000, 0, 600000, false).empty());
    // Long unobservable gap: polling continues (requests still fire) but no
    // eligible time accrues.
    bool polledWhileUnobservable = false;
    for (int64_t t = 1000; t <= 300000; t += 1000)
    {
      if (!poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 60000, 0, 600000, false).empty())
      {
        polledWhileUnobservable = true;
      }
    }
    assert(polledWhileUnobservable);
    // Observable: eligible time accrues 1000 ms per tick from the second
    // observable tick, so at t=900000 each user has accrued EXACTLY 599000
    // ms (one tick short of the 600s window); the window is reached at
    // t=901000, where they must be hidden.
    for (int64_t t = 301000; t <= 900000; t += 1000)
    {
      poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 60000, 0, 600000, true);
    }
    assert(poller.Flush(true));
    assert(poller.Drain());
    {
      const std::string stateMid = ReadFile(statePath);
      const size_t pos = stateMid.find(profile + "\te\t");
      assert(pos != std::string::npos);
      const std::string line = stateMid.substr(pos, stateMid.find('\n', pos) - pos);
      std::vector<std::string> fields;
      size_t fieldStart = 0;
      for (;;)
      {
        const size_t tab = line.find('\t', fieldStart);
        if (tab == std::string::npos)
        {
          fields.push_back(line.substr(fieldStart));
          break;
        }
        fields.push_back(line.substr(fieldStart, tab - fieldStart));
        fieldStart = tab + 1;
      }
      assert(fields.size() == 6);
      assert(fields[3] == "599000"); // exact accrual: no unobservable-gap credit
    }
    for (int64_t t = 901000; t <= 1000000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 60000, 0, 600000, true);
      for (const std::string& u : due)
      {
        assert(u != "e" && u != "f");
      }
    }
    // Newcomer "g" joins after the window would have elapsed: it must not
    // inherit e/f's accrued time. If it had, it would be hidden instantly
    // and never fire; instead it must appear in the poll output.
    std::vector<std::string> withNewcomer = { "e", "f", "g" };
    assert(!Contains(poller.Tick(kBase + 1001000, kBase + kWallOffset + 1001000, profile,
                                 withNewcomer, 60000, 60000, 0, 600000, true), "g"));
    bool gSeen = false;
    for (int64_t t = 1002000; t <= 1300000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, withNewcomer, 60000, 60000, 0, 600000, true);
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
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, many, 60000, 60000, 0, 0, true).empty());
    // One interval later everyone is due, but the per-tick budget caps it.
    std::vector<std::string> due =
      poller.Tick(kBase + 60000, kBase + kWallOffset + 60000, profile, many, 60000, 60000, 0, 0, true);
    assert(due.size() == 3);
    due = poller.Tick(kBase + 61000, kBase + kWallOffset + 61000, profile, many, 60000, 60000, 0, 0, true);
    assert(due.size() == 3);

    // A long gap re-phases overdue users instead of firing a burst.
    PresencePoller paused;
    assert(paused.Init(dir, 43));
    std::vector<std::string> five = { "p0", "p1", "p2", "p3", "p4" };
    assert(paused.Tick(kBase, kBase + kWallOffset, profile, five, 60000, 60000, 0, 0, true).empty());
    due = paused.Tick(kBase + 10 * 60000 + 1000, kBase + kWallOffset + 10 * 60000 + 1000,
                      profile, five, 60000, 60000, 0, 0, true);
    assert(due.empty()); // re-phased, nothing fired at the wake-up tick
    std::unordered_set<std::string> fired;
    for (int64_t t = 602000; t <= 700000; t += 1000)
    {
      due = paused.Tick(kBase + t, kBase + kWallOffset + t, profile, five, 60000, 60000, 0, 0, true);
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

    assert(poller.Tick(kBase, kBase + kWallOffset, pa, { "u1" }, 60000, 60000, 0, 0, true).empty());
    assert(poller.Tick(kBase, kBase + kWallOffset, pb, { "v1" }, 60000, 60000, 0, 0, true).empty());
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
    assert(poller.Tick(kBase, kBase + kWallOffset, pc, { "u1" }, 60000, 60000, 0, 0, true).empty());
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
    // Crash-tail repair with an unterminated fragment LARGER than one 64 KiB
    // scan chunk: the complete line before it must survive (the old
    // single-chunk scan erased the whole file in this case).
    const std::string pd = "Telegram_d";
    const std::string pathD = dir + "/history." + Hex(pd) + ".jsonl";
    {
      std::ofstream out(pathD, std::ios::binary);
      out << "{\"ts\":999,\"u\":\"old\",\"on\":0,\"seen\":-1,\"recent\":0}\n";
      const std::string torn(120000, 'x'); // > 64 KiB, no newline
      out.write(torn.data(), static_cast<std::streamsize>(torn.size()));
    }

    PresencePoller poller;
    assert(poller.Init(dir, 11));
    assert(poller.Tick(kBase, kBase + kWallOffset, pd, { "u1" }, 60000, 60000, 0, 0, true).empty());
    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, pd, "u1", true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());

    const std::string repaired = ReadFile(pathD);
    assert(repaired.find("{\"ts\":999,\"u\":\"old\",\"on\":0,\"seen\":-1,\"recent\":0}\n") == 0);
    assert(CountLines(repaired) == 2);
    assert(repaired.back() == '\n');
    assert(repaired.find("xxxx") == std::string::npos); // torn fragment removed
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
      out << "Telegram_x\tu6\t123\t45\t789\n";       // missing field (5 fields)
      out << "Telegram_x\tu7\t123\t456\t789\t0\tx\n"; // too many fields
      // Embedded NUL written as raw bytes, so the parser really sees it.
      const std::string nulLine = std::string("Telegram_x\tu8\t123\t456\t789") + '\0' +
        std::string("garbage\t0\n");
      out.write(nulLine.data(), static_cast<std::streamsize>(nulLine.size()));
      out << "Telegram_x\tu9\t123\t9223372036854775600\t0\t0\n"; // near INT64_MAX
      out << "Telegram_x\tu3\textra\n";              // wrong field count
    }

    PresencePoller poller;
    assert(poller.Init(corruptDir, 5));
    assert(poller.IsInitialized());
    // u3 was loaded as hidden; every rejected/malformed row (u2, u4..u8)
    // must instead be a fresh, polled user: each fires at least once within
    // one interval. u9 (near-maximum eligible, no data) is handled
    // separately below.
    std::unordered_set<std::string> seen;
    std::vector<std::string> fresh = { "u2", "u4", "u5", "u6", "u7", "u8" };
    for (int64_t t = 6000; t <= 120000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, "Telegram_x",
                    { "u2", "u3", "u4", "u5", "u6", "u7", "u8" }, 60000, 60000, 0, 0, true);
      for (const std::string& u : due)
      {
        assert(u != "u3");
        seen.insert(u);
      }
    }
    for (const std::string& u : fresh)
    {
      assert(seen.count(u) == 1);
    }

    // Rejected rows carry no observable data, so with hidden detection
    // enabled they hide at the fresh-user deadline; if any malformed row
    // had been silently ACCEPTED with its bogus dataMs, it would never
    // hide. u8 (embedded NUL) is the sharpest case: accepted => dataMs=789.
    for (int64_t t = 121000; t <= 200000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, "Telegram_x",
                    { "u2", "u4", "u5", "u6", "u7", "u8" }, 60000, 60000, 0, 60000, true);
      for (const std::string& u : due)
      {
        (void)u;
        assert(u != "u2" && u != "u4" && u != "u5" && u != "u6" && u != "u7" && u != "u8");
      }
    }

    // Near-INT64_MAX eligible time must saturate, not overflow: with hidden
    // detection disabled, two observable ticks accrue a delta onto the
    // loaded near-maximum value.
    {
      PresencePoller saturator;
      assert(saturator.Init(corruptDir, 6));
      std::vector<std::string> candidates = { "u9" };
      assert(saturator.Tick(kBase, kBase + kWallOffset, "Telegram_x", candidates, 60000, 60000, 0, 0, true).empty());
      saturator.Tick(kBase + 1000, kBase + kWallOffset + 1000, "Telegram_x", candidates, 60000, 60000, 0, 0, true);
      saturator.Tick(kBase + 2000, kBase + kWallOffset + 2000, "Telegram_x", candidates, 60000, 60000, 0, 0, true);
      assert(saturator.Flush(true));
      assert(saturator.Drain());
      const std::string state = ReadFile(corruptDir + "/state");
      const size_t pos = state.find("Telegram_x\tu9\t");
      assert(pos != std::string::npos);
      const std::string line = state.substr(pos, state.find('\n', pos) - pos);
      std::vector<std::string> fields;
      size_t fieldStart = 0;
      for (;;)
      {
        const size_t tab = line.find('\t', fieldStart);
        if (tab == std::string::npos)
        {
          fields.push_back(line.substr(fieldStart));
          break;
        }
        fields.push_back(line.substr(fieldStart, tab - fieldStart));
        fieldStart = tab + 1;
      }
      assert(fields.size() == 6);
      assert(fields[3] == "9223372036854775807"); // saturated at INT64_MAX
    }

    // An unreadable state file fails initialization and keeps the poller
    // inert instead of overwriting the file with an empty reconstruction.
    assert(chmod((corruptDir + "/state").c_str(), 0000) == 0);
    PresencePoller blocked;
    assert(!blocked.Init(corruptDir, 5));
    assert(!blocked.IsInitialized());
    assert(blocked.Tick(kBase, kBase + kWallOffset, "Telegram_x", { "u2" }, 60000, 60000, 0, 0, true).empty());
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
      assert(poller.Tick(kBase, kBase + kWallOffset, "Telegram_z", { "z1" }, 60000, 60000, 0, 0, true).empty());
      assert(poller.Flush(true));
      assert(poller.Drain());
      assert(ReadFile(transientState).find("Telegram_z\tz1\t") != std::string::npos);

      // Make the state save fail (no mkostemp possible), add a new user so a
      // fresh state snapshot is forced, and confirm the worker reports it.
      assert(chmod(transientDir.c_str(), 0500) == 0);
      poller.Tick(kBase + 1000, kBase + kWallOffset + 1000, "Telegram_z", { "z1", "z2" }, 60000, 60000, 0, 0, true);
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
    // through the failure-tolerant worker path and reports the loss. The
    // device stays failing THROUGH the shutdown call itself.
    {
      PresencePoller poller;
      assert(poller.Init(transientDir, 22));
      poller.Tick(kBase, kBase + kWallOffset, "Telegram_z", { "z3" }, 60000, 60000, 0, 0, true);
      assert(poller.Flush(true));
      assert(poller.Drain());

      assert(chmod(transientDir.c_str(), 0500) == 0);
      poller.Tick(kBase + 1000, kBase + kWallOffset + 1000, "Telegram_z", { "z3", "z4" }, 60000, 60000, 0, 0, true);
      assert(poller.Flush(true));
      std::string workerError;
      bool gotError = false;
      for (int i = 0; i < 200 && !gotError; ++i)
      {
        gotError = poller.TakeWorkerError(workerError);
        if (!gotError) usleep(10000);
      }
      assert(gotError);
      poller.Shutdown(); // must return promptly while the save keeps failing
      assert(chmod(transientDir.c_str(), 0700) == 0);
      const std::string after = ReadFile(transientState);
      assert(!after.empty()); // never wiped by the transient failure
      assert(after.find("Telegram_z\tz3\t") != std::string::npos);
    }
  }

  {
    // A hidden user whose chat ranks outside the poll set still recovers
    // when real data arrives: the hidden mark clears even though the user
    // is untracked, and the user returns to the poll set without waiting
    // out a fresh window.
    PresencePoller poller;
    assert(poller.Init(dir, 61));
    std::vector<std::string> candidates = { "r1" };
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, candidates, 60000, 60000, 0, 100000, true).empty());
    for (int64_t t = 1000; t <= 200000; t += 1000)
    {
      poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 60000, 0, 100000, true);
    }
    assert(poller.Flush(true));
    assert(poller.Drain());

    // r1 is now hidden and leaves the candidate list entirely.
    std::vector<std::string> noCandidates;
    poller.Tick(kBase + 201000, kBase + kWallOffset + 201000, profile, noCandidates, 60000, 60000, 0, 100000, true);

    // A pushed observation for the untracked hidden user must clear the mark.
    poller.RecordStatus(kBase + 202000, kBase + kWallOffset + 202000, profile, "r1", true, -1, false);
    assert(poller.Flush(true));
    assert(poller.Drain());

    // Restored to the poll set: r1 is selected again and fires, and its
    // recovery observation made it into the history.
    const std::string historyPath = dir + "/history." + Hex(profile) + ".jsonl";
    assert(ReadFile(historyPath).find("\"u\":\"r1\"") != std::string::npos);
    bool r1Seen = false;
    for (int64_t t = 203000; t <= 300000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, candidates, 60000, 60000, 0, 100000, true);
      if (Contains(due, "r1")) r1Seen = true;
    }
    assert(r1Seen);
  }

  {
    // State fields with tabs/newlines in both profile and user ids survive
    // a reload, and polling continues beyond the detection window (proving
    // the observation really was restored, not just re-registered).
    const std::string trickyProfile = "Telegram_\tx";
    const std::string trickyUser = "x\ty\nz";
    PresencePoller poller;
    assert(poller.Init(dir, 7));
    assert(poller.Tick(kBase, kBase + kWallOffset, trickyProfile, { trickyUser }, 60000, 60000, 0, 120000, true).empty());
    poller.RecordStatus(kBase + 5000, kBase + kWallOffset + 5000, trickyProfile, trickyUser, true, -1, false);
    assert(poller.Flush(true));
    assert(poller.Drain());

    PresencePoller reloaded;
    assert(reloaded.Init(dir, 7));
    bool seenAfterWindow = false;
    for (int64_t t = 1000; t <= 300000; t += 1000)
    {
      std::vector<std::string> due =
        reloaded.Tick(kBase + t, kBase + kWallOffset + t, trickyProfile, { trickyUser }, 60000, 60000, 0, 120000, true);
      if (Contains(due, trickyUser) && t > 120000)
      {
        seenAfterWindow = true;
      }
    }
    assert(seenAfterWindow);
  }

  {
    // Queue cap under failure: more lines than the cap must be dropped and
    // reported, and recovery must write exactly one capped batch.
    const std::string pe = "Telegram_e";
    const std::string pathE = dir + "/history." + Hex(pe) + ".jsonl";
    PresencePoller poller;
    assert(poller.Init(dir, 31));
    assert(poller.Tick(kBase, kBase + kWallOffset, pe, { "q1" }, 60000, 60000, 0, 0, true).empty());
    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, pe, "q1", true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());
    assert(chmod(pathE.c_str(), 0400) == 0);
    for (int64_t i = 1; i <= 20100; ++i)
    {
      poller.RecordStatus(kBase + 2000 + i, kBase + kWallOffset + 2000 + i, pe, "q1",
                          (i % 2) == 0, -1, false);
    }
    assert(poller.TakeDroppedLineCount() >= 100); // oldest lines dropped and reported
    assert(poller.Flush());
    std::string workerError;
    bool gotError = false;
    for (int i = 0; i < 200 && !gotError; ++i)
    {
      gotError = poller.TakeWorkerError(workerError);
      if (!gotError) usleep(10000);
    }
    assert(gotError);
    assert(chmod(pathE.c_str(), 0600) == 0);
    assert(poller.Flush());
    assert(poller.Drain());
    assert(CountLines(ReadFile(pathE)) == 20001); // initial line + one capped batch
  }

  {
    // Shutdown loss reporting must survive an earlier unconsumed error: the
    // blocked-line count is appended to whatever error is pending.
    const std::string pf = "Telegram_f";
    const std::string pathF = dir + "/history." + Hex(pf) + ".jsonl";
    PresencePoller poller;
    assert(poller.Init(dir, 71));
    assert(poller.Tick(kBase, kBase + kWallOffset, pf, { "w1" }, 60000, 60000, 0, 0, true).empty());
    poller.RecordStatus(kBase + 1000, kBase + kWallOffset + 1000, pf, "w1", true, -1, false);
    assert(poller.Flush());
    assert(poller.Drain());

    struct rlimit savedLimitF;
    assert(getrlimit(RLIMIT_FSIZE, &savedLimitF) == 0);
    struct rlimit smallLimitF = savedLimitF;
    smallLimitF.rlim_cur = static_cast<rlim_t>(ReadFile(pathF).size() + 10);
    assert(setrlimit(RLIMIT_FSIZE, &smallLimitF) == 0);
    signal(SIGXFSZ, SIG_IGN);
    PresencePoller::TestFailTruncate() = true;
    poller.RecordStatus(kBase + 2000, kBase + kWallOffset + 2000, pf, "w1", false, 1234567890000LL, false);
    assert(poller.Flush());
    usleep(100000); // the worker fails within milliseconds; the error stays UNREAD
    poller.Shutdown(); // stop while the device keeps failing and a line is blocked
    const std::string shutdownError = poller.GetError();
    assert(shutdownError.find("blocked lines lost") != std::string::npos);
    PresencePoller::TestFailTruncate() = false;
    assert(setrlimit(RLIMIT_FSIZE, &savedLimitF) == 0);
    unlink(pathF.c_str()); // the blocked rollback left a torn tail; drop the file
  }

  {
    // Tiered scheduling: the first p_FastCount candidates poll on the fast
    // interval, the rest on the slow interval, both with a random phase and
    // jitter. The fast tier re-fires many times while the slow tier barely
    // does.
    PresencePoller poller;
    assert(poller.Init(dir, 77));
    std::vector<std::string> tiered;
    for (int i = 0; i < 50; ++i)
    {
      tiered.push_back("t" + std::to_string(i));
    }
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, tiered,
                       120000, 5000, 10, 0, true).empty());

    std::unordered_map<std::string, int64_t> firstFire;
    std::unordered_map<std::string, int64_t> lastFire;
    std::unordered_map<std::string, int> fireCount;
    for (int64_t t = 1000; t <= 125000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, tiered,
                    120000, 5000, 10, 0, true);
      assert(due.size() <= 3); // dispatch budget holds in every tick
      for (const std::string& u : due)
      {
        if (!firstFire.count(u))
        {
          firstFire[u] = kBase + t;
        }
        const bool fast = std::stoi(u.substr(1)) < 10;
        const int64_t interval = fast ? 5000 : 120000;
        if (lastFire.count(u))
        {
          // minimum gap: interval minus the +/-20% jitter, exact
          assert(kBase + t - lastFire[u] >= interval - interval / 5);
        }
        lastFire[u] = kBase + t;
        fireCount[u] += 1;
      }
    }
    assert(firstFire.size() == 50); // all tiers drained within the window
    // Fast tier cadence: roughly every 5s over 120s.
    for (int i = 0; i < 10; ++i)
    {
      const std::string u = "t" + std::to_string(i);
      assert(fireCount[u] >= 15); // ~24 fires expected; plenty of margin
    }
    // Slow tier cadence: at most two fires within 125s (phase + one
    // reschedule after 120s+jitter is beyond the window).
    for (int i = 10; i < 50; ++i)
    {
      const std::string u = "t" + std::to_string(i);
      assert(fireCount[u] <= 2);
    }
  }

  {
    // Tier promotion: a slow contact promoted into the fast tier has its
    // deadline brought forward instead of waiting out the slow schedule.
    PresencePoller poller;
    assert(poller.Init(dir, 78));
    std::vector<std::string> slowFirst = { "s1", "s2" }; // all slow: no fast tier
    assert(poller.Tick(kBase, kBase + kWallOffset, profile, slowFirst,
                       120000, 5000, 0, 0, true).empty());
    // Promote s1 into the fast tier at t=3000: candidates reordered.
    std::vector<std::string> promoted = { "s1", "s2" };
    bool s1Seen = false;
    for (int64_t t = 4000; t <= 10000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, kBase + kWallOffset + t, profile, promoted,
                    120000, 5000, 1, 0, true);
      for (const std::string& u : due)
      {
        if (u == "s1") s1Seen = true;
      }
    }
    assert(s1Seen); // promoted contact sampled within the fast interval
  }

  if (ownDir)
  {
    const std::string cleanup = "rm -rf " + dir;
    assert(std::system(cleanup.c_str()) == 0);
  }

  std::printf("test-presence-poller: all checks passed\n");
  return 0;
}
