// test-presence-poller.cpp
//
// Standalone regression test for PresencePoller (header-only). Exercises
// recording/coalescing/heartbeats, gating, randomized scheduling with the
// dispatch budget and re-phasing, eligible-time hidden detection, hidden
// recovery, persistence across reloads, flush failure rollback (no duplicate
// lines), crash-tail repair, corrupt state handling and init failure.
//
// Uses a private temp directory; no real protocols, accounts or message data.
// Run with an optional base directory argument to keep the generated files
// for external validation (the shell wrapper validates the JSONL with
// python3 and removes the directory afterwards).

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>

#include "../src/presencepoller.h"

static const int64_t kBase = 1000000000; // wall-clock-ish base ms for records

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
    // Recording, coalescing, heartbeat re-affirmation and gating.
    PresencePoller poller;
    assert(poller.Init(dir, 12345));
    assert(poller.IsInitialized());

    const std::string specialUser = "u\"x\\\ty";
    std::vector<std::string> candidates = { "u1", specialUser };
    assert(poller.Tick(kBase, profile, candidates, 60000, 120000).empty());

    poller.RecordStatus(kBase + 1000, profile, "u1", false, -1, false);
    assert(poller.Flush());

    const std::string historyPath = dir + "/history." + Hex(profile) + ".jsonl";
    std::string history = ReadFile(historyPath);
    assert(CountLines(history) == 1);
    assert(history.find("\"ts\":1000001000") != std::string::npos);
    assert(history.find("\"u\":\"u1\"") != std::string::npos);
    assert(history.find("\"on\":0") != std::string::npos);
    assert(history.find("\"seen\":-1") != std::string::npos);
    assert(history.find("\"recent\":0") != std::string::npos);
    assert(history.find("\"hb\"") == std::string::npos);

    // Identical consecutive observation is coalesced.
    poller.RecordStatus(kBase + 2000, profile, "u1", false, -1, false);
    assert(poller.Flush());
    assert(CountLines(ReadFile(historyPath)) == 1);

    // A state change is always recorded.
    poller.RecordStatus(kBase + 3000, profile, "u1", true, -1, false);
    if (!poller.Flush())
    {
      std::fprintf(stderr, "flush3 failed: %s\n", poller.GetError().c_str());
      assert(false);
    }
    assert(CountLines(ReadFile(historyPath)) == 2);

    // Heartbeat: no-op inside the window, re-affirmation after it.
    poller.RecordHeartbeat(kBase + 4000, profile, "u1", 600000);
    assert(poller.Flush());
    assert(CountLines(ReadFile(historyPath)) == 2);
    poller.RecordHeartbeat(kBase + 700000, profile, "u1", 600000);
    assert(poller.Flush());
    history = ReadFile(historyPath);
    assert(CountLines(history) == 3);
    assert(history.find("\"hb\":1") != std::string::npos);

    // Users outside the poll set are never recorded.
    poller.RecordStatus(kBase + 800000, profile, "uOut", true, -1, false);
    assert(poller.Flush());
    assert(CountLines(ReadFile(historyPath)) == 3);

    // JSON escaping of special characters in user ids.
    poller.RecordStatus(kBase + 900000, profile, specialUser, false, -1, false);
    assert(poller.Flush());
    history = ReadFile(historyPath);
    assert(history.find("\\\"x\\\\\\ty") != std::string::npos);

    // A user that drops out of the candidate list stops being recorded.
    std::vector<std::string> onlyU1 = { "u1" };
    poller.Tick(kBase + 1000000, profile, onlyU1, 60000, 120000);
    poller.RecordStatus(kBase + 1001000, profile, specialUser, true, -1, false);
    assert(poller.Flush());
    history = ReadFile(historyPath);
    assert(CountOccurrences(history, "\"u\":\"u\\\"x\\\\\\ty\"") == 1);

    // Hidden marker line.
    poller.RecordHiddenMark(kBase + 1100000, profile, "u1");
    assert(poller.Flush());
    history = ReadFile(historyPath);
    assert(history.find("\"mark\":\"hidden\"") != std::string::npos);
  }

  {
    // Scheduling, eligible-time hidden detection, recovery and persistence.
    PresencePoller poller;
    assert(poller.Init(dir, 999));

    std::vector<std::string> candidates = { "a", "b", "c", "d" };
    assert(poller.Tick(kBase, profile, candidates, 60000, 120000).empty());
    poller.RecordStatus(kBase + 1000, profile, "b", true, -1, false);

    // Spread across the interval, at least 48s between consecutive fires.
    std::unordered_map<std::string, int64_t> lastFire;
    for (int64_t t = 1000; t <= 80000; t += 1000)
    {
      std::vector<std::string> due = poller.Tick(kBase + t, profile, candidates, 60000, 120000);
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
      std::vector<std::string> due = poller.Tick(kBase + t, profile, candidates, 60000, 120000);
      for (const std::string& u : due)
      {
        if (kBase + t >= kBase + 120000)
        {
          assert(u != "a" && u != "c" && u != "d");
        }
        if (u == "b") bSeen = true;
      }
    }
    assert(bSeen);
    if (!poller.Flush())
    {
      std::fprintf(stderr, "flush failed: %s\n", poller.GetError().c_str());
      assert(false);
    }

    // The state file persisted hidden=1 for a/c/d with no data, and b
    // un-hidden.
    const std::string state = ReadFile(statePath);
    assert(state.find(profile + "\tb\t") != std::string::npos);
    for (const std::string u : { "a", "c", "d" })
    {
      const size_t pos = state.find(profile + "\t" + u + "\t");
      assert(pos != std::string::npos);
      assert(state.find("\t0\t1\n", pos) != std::string::npos); // data=0, hidden=1
    }

    // Observable data for a hidden user returns it to the poll set.
    poller.RecordStatus(kBase + 150000, profile, "c", true, -1, false);
    bool cSeen = false;
    for (int64_t t = 151000; t <= 230000; t += 1000)
    {
      std::vector<std::string> due = poller.Tick(kBase + t, profile, candidates, 60000, 120000);
      for (const std::string& u : due)
      {
        assert(u != "a" && u != "d");
        if (u == "c") cSeen = true;
      }
    }
    assert(cSeen);
    assert(poller.Flush());

    // Reload from disk: hidden marks and the recovery survive.
    PresencePoller reloaded;
    assert(reloaded.Init(dir, 999));
    for (int64_t t = 231000; t <= 320000; t += 1000)
    {
      std::vector<std::string> due = reloaded.Tick(kBase + t, profile, candidates, 60000, 120000);
      for (const std::string& u : due)
      {
        assert(u != "a" && u != "d");
      }
    }
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
    assert(poller.Tick(kBase, profile, many, 60000, 0).empty());
    // One interval later everyone is due, but the per-tick budget caps it.
    std::vector<std::string> due = poller.Tick(kBase + 60000, profile, many, 60000, 0);
    assert(due.size() == 3);
    due = poller.Tick(kBase + 61000, profile, many, 60000, 0);
    assert(due.size() == 3);

    // A long gap re-phases overdue users instead of firing a burst.
    PresencePoller paused;
    assert(paused.Init(dir, 43));
    std::vector<std::string> five = { "p0", "p1", "p2", "p3", "p4" };
    assert(paused.Tick(kBase, profile, five, 60000, 0).empty());
    due = paused.Tick(kBase + 10 * 60000 + 1000, profile, five, 60000, 0);
    assert(due.empty()); // re-phased, nothing fired at the wake-up tick
    std::unordered_set<std::string> fired;
    for (int64_t t = 602000; t <= 700000; t += 1000)
    {
      due = paused.Tick(kBase + t, profile, five, 60000, 0);
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

    assert(poller.Tick(kBase, pa, { "u1" }, 60000, 0).empty());
    assert(poller.Tick(kBase, pb, { "v1" }, 60000, 0).empty());
    poller.RecordStatus(kBase + 1000, pa, "u1", true, -1, false);
    poller.RecordStatus(kBase + 2000, pb, "v1", true, -1, false);
    assert(poller.Flush());

    const std::string pathA = dir + "/history." + Hex(pa) + ".jsonl";
    const std::string pathB = dir + "/history." + Hex(pb) + ".jsonl";
    assert(CountLines(ReadFile(pathA)) == 1);
    assert(CountLines(ReadFile(pathB)) == 1);

    // Make profile A's file unwritable, then record a change for both.
    assert(chmod(pathA.c_str(), 0400) == 0);
    poller.RecordStatus(kBase + 3000, pa, "u1", false, 1234567890000LL, false);
    poller.RecordStatus(kBase + 3000, pb, "v1", false, 1234567890000LL, false);
    assert(!poller.Flush());

    // B committed; A kept its single line and its pending line is retained.
    assert(CountLines(ReadFile(pathB)) == 2);
    assert(CountLines(ReadFile(pathA)) == 1);

    // Restore and retry: A gets exactly one copy of the pending line.
    assert(chmod(pathA.c_str(), 0600) == 0);
    assert(poller.Flush());
    const std::string aHistory = ReadFile(pathA);
    assert(CountLines(aHistory) == 2);
    assert(CountOccurrences(aHistory, "\"seen\":1234567890000") == 1);

    // State save failure does not lose history and is retried.
    poller.Tick(kBase + 5000, pb, { "v1", "v2" }, 60000, 0); // new user -> dirty state
    assert(chmod(dir.c_str(), 0500) == 0);
    poller.RecordStatus(kBase + 4000, pb, "v1", true, -1, false);
    assert(!poller.Flush()); // history ok, state save (mkstemp) failed
    assert(chmod(dir.c_str(), 0700) == 0);
    assert(poller.Flush());
    assert(ReadFile(statePath).find("Telegram_b\tv2\t") != std::string::npos);
  }

  {
    // Crash-tail repair: a partial trailing line is truncated before the
    // next append, and complete lines are preserved.
    const std::string pc = "Telegram_c";
    const std::string pathC = dir + "/history." + Hex(pc) + ".jsonl";
    {
      std::ofstream out(pathC, std::ios::binary);
      out << "{\"ts\":999,\"u\":\"old\",\"on\":0,\"seen\":-1,\"recent\":0}\n{\"ts\":100"; // complete + torn
    }

    PresencePoller poller;
    assert(poller.Init(dir, 11));
    assert(poller.Tick(kBase, pc, { "u1" }, 60000, 0).empty());
    poller.RecordStatus(kBase + 1000, pc, "u1", true, -1, false);
    assert(poller.Flush());

    const std::string repaired = ReadFile(pathC);
    assert(repaired.find("{\"ts\":999,\"u\":\"old\",\"on\":0,\"seen\":-1,\"recent\":0}\n") == 0);
    assert(CountLines(repaired) == 2);
    assert(repaired.back() == '\n');
    assert(repaired.find("100{\"ts\":") == std::string::npos); // torn fragment removed
  }

  {
    // Corrupt state lines are skipped; an unreadable state file fails
    // initialization and keeps the poller inert instead of overwriting it.
    std::string corruptDir = dir + "/corrupt";
    mkdir(corruptDir.c_str(), 0700);
    {
      std::ofstream out(corruptDir + "/state", std::ios::binary);
      out << "garbage line\n";
      out << "Telegram_x\tu\tabc\t1\t2\t3\n";        // non-numeric first
      out << "Telegram_x\tu2\t123\t456\t789\t7\n";   // invalid hidden value
      out << "Telegram_x\tu3\t123\t456\t789\t1\n";   // valid hidden=1
      out << "Telegram_x\tu3\textra\n";              // wrong field count
    }

    PresencePoller poller;
    assert(poller.Init(corruptDir, 5));
    assert(poller.IsInitialized());
    // u3 was loaded as hidden; u2 is fresh and polled normally.
    bool u2Seen = false;
    for (int64_t t = 6000; t <= 200000; t += 1000)
    {
      std::vector<std::string> due =
        poller.Tick(kBase + t, "Telegram_x", { "u2", "u3" }, 60000, 0);
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
    assert(blocked.Tick(kBase, "Telegram_x", { "u2", "u3" }, 60000, 0).empty());
    blocked.RecordStatus(kBase, "Telegram_x", "u2", true, -1, false);
    assert(!blocked.Flush());
    assert(chmod((corruptDir + "/state").c_str(), 0600) == 0);
    PresencePoller retried;
    assert(retried.Init(corruptDir, 5));
    assert(retried.IsInitialized());
  }

  {
    // State fields with tabs/newlines in both profile and user ids survive
    // a reload.
    const std::string trickyProfile = "Telegram_\tx";
    const std::string trickyUser = "x\ty\nz";
    PresencePoller poller;
    assert(poller.Init(dir, 7));
    assert(poller.Tick(kBase, trickyProfile, { trickyUser }, 60000, 120000).empty());
    poller.RecordStatus(kBase + 5000, trickyProfile, trickyUser, true, -1, false);
    assert(poller.Flush());

    PresencePoller reloaded;
    assert(reloaded.Init(dir, 7));
    bool seen = false;
    for (int64_t t = 1000; t <= 200000; t += 1000)
    {
      std::vector<std::string> due =
        reloaded.Tick(kBase + t, trickyProfile, { trickyUser }, 60000, 120000);
      if (Contains(due, trickyUser))
      {
        seen = true;
      }
    }
    assert(seen); // observable data survived the round trip (never hidden)
  }

  if (ownDir)
  {
    const std::string cleanup = "rm -rf " + dir;
    assert(std::system(cleanup.c_str()) == 0);
  }

  std::printf("test-presence-poller: all checks passed\n");
  return 0;
}
