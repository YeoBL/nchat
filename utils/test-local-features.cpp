// clang++ -std=c++14 -Ilib/ncutil/src utils/test-local-features.cpp -o /tmp/nchat-local-features-test && /tmp/nchat-local-features-test
#include "../src/localfeatures.h"
#include <cassert>
#include <fstream>
#include <iostream>

int main()
{
  char directory[] = "/tmp/nchat-local-features-XXXXXX";
  assert(mkdtemp(directory));
  const std::string path = std::string(directory) + "/state";
  const LocalFeatures::Chat tg{"Telegram_test", "same-chat"}, wa{"WhatsApp_test", "same-chat"};
  const std::string draft = "Birthday 🎂\nUnicode: 中文\n\nLast line\t";
  {
    LocalFeatures state(path);
    assert(state.IsWritable());
    assert(state.SetDraft(tg, draft));
    assert(state.SetDraft(wa, "Other profile"));
    assert(state.SetDraft(tg, "Deferred 🎂\ntext", false));
    assert(state.GetDraft(tg) == "Deferred 🎂\ntext");
    LocalFeatures observer(path);
    assert(observer.GetDraft(tg) == draft);
    assert(state.Flush());
    assert(observer.Load() && observer.GetDraft(tg) == "Deferred 🎂\ntext");
    assert(state.SetDraft(tg, draft));
    assert(state.CreateFolder("Empty folder"));
    assert(state.SetFolder(tg, "Friends 🎉", true));
    assert(state.SetFolder(wa, "Friends 🎉", true));
    assert(state.SetBookmark(tg, "message", "Saved\nmessage 🐈", 1234567890123));
    assert(state.SetBookmark(wa, "message", "Different account", 10));
    struct stat info{};
    assert(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
  }
  {
    LocalFeatures state(path);
    assert(state.GetDraft(tg) == draft && state.GetDraft(wa) == "Other profile");
    assert(state.GetFolders().size() == 2 && state.GetChatFolders(tg).size() == 1);
    assert(state.GetFolderChats("Friends 🎉").size() == 2);
    assert(state.GetBookmarks().size() == 2);
    assert(state.GetBookmarks()[0].text == "Saved\nmessage 🐈");
    assert(state.GetBookmarks()[0].time == 1234567890123);
    assert(state.SetFolder(tg, "Friends 🎉", false));
    assert(!state.InFolder(tg, "Friends 🎉") && state.InFolder(wa, "Friends 🎉"));
    assert(state.SetDraft(tg, ""));
    assert(state.RemoveBookmark(tg, "message"));
    assert(!state.IsBookmarked(tg, "message") && state.IsBookmarked(wa, "message"));
    assert(state.DeleteFolder("Empty folder"));
  }
  {
    LocalFeatures state(path);
    assert(state.GetDraft(tg).empty() && state.GetDraft(wa) == "Other profile");
    assert(state.GetFolders().size() == 1 && state.GetBookmarks().size() == 1);
    // A failed write must keep the in-memory snapshot and existing file intact.
    const std::string moved = std::string(directory) + "/moved";
    assert(rename(path.c_str(), moved.c_str()) == 0);
    assert(mkdir(path.c_str(), 0700) == 0);
    assert(!state.SetDraft(wa, "Should not persist"));
    assert(state.GetDraft(wa) == "Other profile");
    assert(state.SetDraft(wa, "Pending after failed flush", false));
    assert(!state.Flush());
    assert(state.GetDraft(wa) == "Pending after failed flush");
    assert(rmdir(path.c_str()) == 0);
    assert(rename(moved.c_str(), path.c_str()) == 0);
    assert(state.Flush());
    LocalFeatures observer(path);
    assert(observer.GetDraft(wa) == "Pending after failed flush");
    assert(state.SetDraft(wa, "Other profile"));
    // Corrupt storage is reported and never overwritten by a new edit.
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << "NCHATLF1\0"; }
    assert(!state.Load() && !state.IsWritable());
    assert(state.GetDraft(wa) == "Other profile");
    assert(!state.SetDraft(tg, "Never overwrite corrupt storage"));
    std::ifstream file(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    assert(bytes == "NCHATLF1");
  }
  assert(unlink(path.c_str()) == 0);
  assert(rmdir(directory) == 0);
  std::cout << "local drafts, deferred flush, folders, bookmarks, account isolation, private writes and corruption handling: passed\n";
}
