// Uses an in-memory database only; never opens a user profile.
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "protocol.h"
#include <sqlite_modern_cpp.h>
#define private public
#include "messagecache.h"
#undef private
#include <cassert>
#include <iostream>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include "appconfig.h"
#include "fileutil.h"

int main() try
{
  auto& db = MessageCache::m_Dbs["test"];
  db = std::make_unique<sqlite::database>(":memory:");
  *db << "CREATE TABLE messages(chatId TEXT, id TEXT, isOutgoing INT, isRead INT);";
  *db << "INSERT INTO messages VALUES ('chat','3939',0,0),('chat','313030',0,0),"
         "('chat','313031',1,0),('other','3939',0,0);";
  auto request = std::make_shared<MessageCache::UpdateMessageIsReadRequest>();
  request->profileId = "test";
  request->chatId = "chat";
  request->isRead = true;
  request->readThroughId = "3939"; // Read through 99, not 100.
  MessageCache::PerformRequest(request);
  int count = -1;
  *db << "SELECT count(*) FROM messages WHERE isRead=1;" >> count;
  assert(count == 1);
  request->readThroughId.clear(); // Whole incoming chat.
  MessageCache::PerformRequest(request);
  *db << "SELECT count(*) FROM messages WHERE isRead=1;" >> count;
  assert(count == 2); // Outgoing messages and other chats are unaffected.
  request->msgId = "313031";
  MessageCache::PerformRequest(request);
  *db << "SELECT count(*) FROM messages WHERE isRead=1;" >> count;
  assert(count == 3);
  MessageCache::m_Dbs.clear();
  std::cout << "production cache: partial, whole-chat, and message reads passed\n";

  // Download callbacks may finish after a message has been redacted or retained.
  for (const std::string& profile : {"files", "other-files"})
  {
    auto& files = MessageCache::m_Dbs[profile];
    files = std::make_unique<sqlite::database>(":memory:");
    *files << "CREATE TABLE messages(chatId TEXT, id TEXT, isDeleted INT, fileInfo TEXT, "
              "text TEXT DEFAULT 'cached text', isRead INT DEFAULT 1);";
    *files << "INSERT INTO messages(chatId,id,isDeleted,fileInfo) VALUES ('chat','redacted',1,''),('chat','redacted-null',1,NULL),"
              "('chat','retained',1,'original-file'),('chat','normal',0,''),"
              "('chat','normal-null',NULL,NULL),('other','retained',1,'other-chat-file');";
  }
  auto fileUpdate = std::make_shared<MessageCache::UpdateMessageFileInfoRequest>();
  fileUpdate->profileId = "files";
  fileUpdate->chatId = "chat";
  fileUpdate->fileInfo = "downloaded-file";
  for (const std::string& id : {"redacted", "redacted-null", "retained", "normal", "normal-null", "missing"})
  {
    fileUpdate->msgId = id;
    MessageCache::PerformRequest(fileUpdate);
  }
  auto& files = *MessageCache::m_Dbs.at("files");
  files << "SELECT count(*) FROM messages WHERE chatId='chat' AND id LIKE 'redacted%' "
           "AND isDeleted=1 AND COALESCE(fileInfo,'')='';" >> count;
  assert(count == 2); // Stale media cannot reattach to redacted tombstones, including NULL metadata.
  files << "SELECT count(*) FROM messages WHERE chatId='chat' "
           "AND id IN ('retained','normal','normal-null') AND fileInfo='downloaded-file';" >> count;
  assert(count == 3); // Mode 3 retained media and ordinary downloads can still finish.
  files << "SELECT isDeleted FROM messages WHERE chatId='chat' AND id='retained';" >> count;
  assert(count == 1);
  files << "SELECT count(*) FROM messages WHERE chatId='other' AND id='retained' AND fileInfo='other-chat-file';" >> count;
  assert(count == 1);
  *MessageCache::m_Dbs.at("other-files") << "SELECT count(*) FROM messages WHERE chatId='chat' "
    "AND id='retained' AND fileInfo='original-file';" >> count;
  assert(count == 1); // Same message ids in another chat/account are unaffected.
  files << "SELECT count(*) FROM messages;" >> count;
  assert(count == 6); // An unknown message callback must not insert a record.
  const auto overlays = MessageCache::GetDeletedMessageOverlays("files", "chat",
    {"retained", "redacted", "redacted-null", "normal", "missing"});
  assert(overlays.size() == 3);
  assert(overlays.at("retained").id == "retained" && overlays.at("retained").isDeleted);
  assert(overlays.at("retained").text == "cached text" && overlays.at("retained").isRead);
  assert(overlays.at("retained").fileInfo == "downloaded-file");
  assert(overlays.at("redacted").fileInfo.empty() && overlays.at("redacted-null").fileInfo.empty());
  assert(MessageCache::GetDeletedMessageOverlays("files", "other", {"retained"}).at("retained").fileInfo == "other-chat-file");
  assert(MessageCache::GetDeletedMessageOverlays("other-files", "chat", {"retained"}).at("retained").fileInfo == "original-file");
  assert(MessageCache::GetDeletedMessageOverlays("files", "other' OR 1=1 --", {"retained"}).empty());
  assert(MessageCache::GetDeletedMessageOverlays("files", "chat", {"retained' OR 1=1 --"}).empty());
  files << "INSERT INTO messages VALUES('chat','000-first',1,'','first chunk',1),"
    "('chat','zz-last',1,'','last chunk',1);";
  for (int i = 0; i < 700; ++i)
    files << "INSERT INTO messages(chatId,id,isDeleted,fileInfo,text,isRead) VALUES('chat',?,1,'',?,1);"
      << ("unrelated-deleted-" + std::to_string(i)) << std::string(4096, 'x');
  std::set<std::string> manyIds{"000-first", "retained", "redacted", "redacted-null", "zz-last"};
  for (int i = 0; i < 1200; ++i) manyIds.insert("not-cached-" + std::to_string(i));
  const auto manyOverlays = MessageCache::GetDeletedMessageOverlays("files", "chat", manyIds);
  assert(manyOverlays.size() == 5 && manyOverlays.at("retained").text == "cached text" &&
    manyOverlays.at("000-first").text == "first chunk" && manyOverlays.at("zz-last").text == "last chunk");
  const auto profileCount = MessageCache::m_Dbs.size();
  assert(MessageCache::GetDeletedMessageOverlays("missing", "chat", {"retained"}).empty());
  assert(MessageCache::GetDeletedMessageOverlays("files", "chat", {}).empty());
  assert(MessageCache::m_Dbs.size() == profileCount);
  const bool cacheEnabled = MessageCache::m_CacheEnabled;
  MessageCache::m_CacheEnabled = false;
  assert(MessageCache::GetDeletedMessageOverlays("files", "chat", {"retained"}).empty());
  MessageCache::m_CacheEnabled = cacheEnabled;
  MessageCache::m_Dbs.clear();
  std::cout << "production cache: late downloads preserve deletion redaction, retained media, and chat/account isolation\n";
  std::cout << "production cache: batch deleted overlays preserve redaction, restrict ids, and isolate chat/account queries\n";

  // Real on-disk production initialization and COMMIT vs. external reader.
  char temporary[] = "/tmp/nchat-cache-lock-test.XXXXXX";
  const char* directory = mkdtemp(temporary);
  assert(directory);
  FileUtil::SetApplicationDir(directory);
  AppConfig::Init();
  MessageCache::Init();
  MessageCache::AddProfile("test", false, 1, false, false);
  auto& writer = *MessageCache::m_Dbs.at("test");
  int timeout = 0;
  writer << "PRAGMA busy_timeout;" >> timeout;
  assert(timeout == 5000);
  writer << "INSERT INTO messages(chatId,id,isOutgoing,isRead) VALUES('chat','1',0,0);";
  assert(MessageCache::GetUnreadMessageIds("test", "chat") == std::vector<std::string>{"1"});
  assert(MessageCache::GetUnreadMessageIds("test", "other' OR 1=1 --").empty());
  assert(MessageCache::GetUnreadMessageIds("missing", "chat").empty());
  assert(MessageCache::GetUnreadChatIds("test") == std::vector<std::string>{"chat"});
  auto membership = std::make_shared<MessageCache::UpdateMembershipRequest>();
  membership->profileId = "test"; membership->chatId = "chat";
  int membershipNotifications = 0;
  MessageCache::SetMessageHandler([&](std::shared_ptr<ServiceMessage> event) {
    assert(event->GetMessageType() == UpdateMembershipNotifyType);
    auto notify = std::static_pointer_cast<UpdateMembershipNotify>(event);
    assert(notify->chatId == "chat" && notify->isMember == membership->isMember);
    assert(MessageCache::GetUnreadChatIds("test") == std::vector<std::string>{"chat"}); // Callback outside DB lock.
    ++membershipNotifications;
  });
  for (bool member : {false, true, false})
  {
    membership->isMember = member;
    MessageCache::PerformRequest(membership);
    writer << "SELECT isMember FROM chat_membership WHERE chatId='chat';" >> count;
    assert(count == int(member));
    writer << "SELECT count(*) FROM messages WHERE chatId='chat' AND isRead=0;" >> count;
    assert(count == 1);
  }
  assert(membershipNotifications == 3);
  MessageCache::SetMessageHandler(nullptr);
  std::cout << "production membership: leave/rejoin preserves cached messages and read flags\n";
  auto archived = std::make_shared<MessageCache::UpdateArchivedRequest>();
  archived->profileId = "test";
  archived->chatId = "chat";
  for (bool state : {true, false, true})
  {
    archived->isArchived = state;
    MessageCache::PerformRequest(archived);
    writer << "SELECT isArchived FROM chats2 WHERE id='chat';" >> count;
    assert(count == int(state));
    writer << "SELECT count(*) FROM messages WHERE chatId='chat' AND isRead=0;" >> count;
    assert(count == 1); // Archive updates must preserve history and unread state.
  }
  std::cout << "production cache: archive/unarchive preserves messages and read flags\n";
  {
    // These are real cache requests and real reentrant queries, not UI stubs.
    // A cache callback holding m_DbMutex deterministically blocks on itself;
    // the watchdog makes that regression a bounded failure rather than a hang.
    alarm(10);
    auto add = std::make_shared<MessageCache::AddMessagesRequest>();
    add->profileId = "test"; add->chatId = "reaction-locks";
    ChatMessage first;
    first.id = "first"; first.text = "old first"; first.timeSent = 1000;
    first.isOutgoing = false; first.isRead = false;
    first.reactions.senderEmojis = {{"synthetic-person", "thumbs-up"}};
    first.reactions.updateCountBasedOnSender = true;
    ChatMessage second = first;
    second.id = "second"; second.text = "old second"; second.timeSent = 2000; second.isRead = true;
    add->chatMessages = {first, second};
    MessageCache::PerformRequest(add);
    int callbacks = 0;
    bool expectCompletedBatch = false;
    MessageCache::SetMessageHandler([&](std::shared_ptr<ServiceMessage> event) {
      assert(event->GetMessageType() == NewMessageReactionsNotifyType);
      auto notify = std::static_pointer_cast<NewMessageReactionsNotify>(event);
      assert(notify->chatId == "reaction-locks");
      assert(!notify->reactions.senderEmojis.empty());
      std::vector<ChatMessage> previous;
      assert(MessageCache::GetOneMessage("test", "reaction-locks", "first", previous));
      assert(!previous.front().isRead);
      previous.clear();
      assert(MessageCache::GetOneMessage("test", "reaction-locks", "second", previous));
      assert(previous.front().isRead);
      if (expectCompletedBatch) assert(previous.front().text == "new second");
      assert(MessageCache::GetDeletedMessageOverlays("test", "reaction-locks", {"first", "second"}).empty());
      ++callbacks;
    });
    auto update = std::make_shared<MessageCache::UpdateMessageReactionsRequest>();
    update->profileId = "test"; update->chatId = "reaction-locks"; update->msgId = "first";
    update->reactions.senderEmojis = {{"synthetic-person", "heart"}};
    update->reactions.updateCountBasedOnSender = true;
    MessageCache::PerformRequest(update);
    assert(callbacks == 1);
    // Both old messages already have non-default reactions, exercising the
    // second callback source and requiring all writes before notification.
    first.text = "new first"; second.text = "new second";
    add->chatMessages = {first, second};
    expectCompletedBatch = true;
    MessageCache::PerformRequest(add);
    assert(callbacks == 3);
    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> weakLifetime = lifetime;
    MessageCache::SetMessageHandler([&, held = lifetime](std::shared_ptr<ServiceMessage>) {
      MessageCache::SetMessageHandler(nullptr); // Reentrant replacement cannot destroy this invocation.
      assert(!weakLifetime.expired() && *held == 42);
      std::vector<ChatMessage> previous;
      assert(MessageCache::GetOneMessage("test", "reaction-locks", "first", previous));
      assert(!previous.front().isRead);
    });
    lifetime.reset();
    MessageCache::PerformRequest(update);
    assert(weakLifetime.expired());

    // A real worker may have copied the old UI callback just before unregister.
    // StopWorker must wait for it and commit the final queued writes, without
    // closing databases needed by the later export/cleanup phase.
    std::mutex gate;
    std::condition_variable changed;
    bool entered = false, released = false, uiAlive = true;
    std::atomic<bool> stopped{false};
    int workerCallbacks = 0;
    MessageCache::SetMessageHandler([&](std::shared_ptr<ServiceMessage>) {
      std::unique_lock<std::mutex> hold(gate);
      assert(uiAlive);
      entered = true;
      changed.notify_all();
      changed.wait(hold, [&] { return released; });
      assert(uiAlive);
      std::vector<ChatMessage> previous;
      assert(MessageCache::GetOneMessage("test", "reaction-locks", "first", previous));
      ++workerCallbacks;
    });
    MessageCache::EnqueueRequest(update);
    {
      std::unique_lock<std::mutex> hold(gate);
      assert(changed.wait_for(hold, std::chrono::seconds(2), [&] { return entered; }));
    }
    ChatMessage finalWrite = second;
    finalWrite.id = "last-queued-write";
    MessageCache::AddMessages("test", "reaction-locks", "", {finalWrite});
    MessageCache::SetMessageHandler(nullptr);
    std::thread stop([&] { MessageCache::StopWorker(); stopped = true; });
    while (true)
    {
      std::unique_lock<std::mutex> hold(MessageCache::m_QueueMutex);
      if (!MessageCache::m_Running) break;
      hold.unlock();
      std::this_thread::yield();
    }
    assert(!stopped); // In-flight callback still owns the simulated UI lifetime.
    {
      std::unique_lock<std::mutex> hold(gate);
      released = true;
      changed.notify_all();
    }
    stop.join();
    assert(stopped && workerCallbacks == 1);
    uiAlive = false;
    std::vector<ChatMessage> finalStored;
    assert(MessageCache::GetOneMessage("test", "reaction-locks", finalWrite.id, finalStored));
    assert(finalStored.front().isRead && MessageCache::m_Queue.empty());
    finalWrite.id = "rejected-after-stop";
    MessageCache::AddMessages("test", "reaction-locks", "", {finalWrite});
    finalStored.clear();
    assert(!MessageCache::GetOneMessage("test", "reaction-locks", finalWrite.id, finalStored));
    assert(MessageCache::m_Queue.empty());
    MessageCache::StopWorker(); // Lifecycle calls remain idempotent.
    writer << "DELETE FROM messages WHERE chatId='reaction-locks';";
    alarm(0);
  }
  std::cout << "production callbacks: both reaction paths permit cache reentry, preserve read flags, complete batch writes and safely replace the active handler\n";
  std::cout << "production lifecycle: unregister/stop drains in-flight callbacks and queued writes before UI destruction, retaining databases for export\n";
  auto receipt = std::make_shared<MessageCache::MessageReceiptsRequest>();
  receipt->profileId = "test"; receipt->chatId = "chat"; receipt->msgId = "sent";
  receipt->receipts = {{"alice", 0, 200}, {"bob", 100, 0}};
  MessageCache::PerformRequest(receipt);
  receipt->receipts = {{"alice", 150, 210}, {"alice", 160, 0}, {"bob", 0, 220}, {"empty", 0, 0}};
  MessageCache::PerformRequest(receipt);
  receipt->onResult = [](MessageReceiptInfo info) {
    assert(info.receipts.size() == 2);
    assert(info.receipts[0].userId == "alice" && info.receipts[0].deliveredTime == 150 && info.receipts[0].readTime == 200);
    assert(info.receipts[1].userId == "bob" && info.receipts[1].deliveredTime == 100 && info.receipts[1].readTime == 220);
    assert(info.note.find("incomplete") != std::string::npos);
  };
  MessageCache::PerformRequest(receipt);
  receipt->chatId = "other' OR 1=1 --";
  receipt->onResult = [](MessageReceiptInfo info) { assert(info.receipts.empty()); };
  MessageCache::PerformRequest(receipt);
  writer << "SELECT count(*) FROM messages WHERE chatId='chat' AND isRead=0;" >> count;
  assert(count == 1);
  std::cout << "production receipts: duplicate/out-of-order merge, recipient isolation, unknown dates, SQL binding and unchanged read flags passed\n";
  {
    // Old rows predate the phone/LID mapping; merge only verified aliases at
    // read time so an unavailable mapping cannot silently erase history.
    MessageReceiptInfo info{{{"123@lid", 0, 200}, {"447700900123@s.whatsapp.net", 150, 0},
      {"447700900123:2@s.whatsapp.net", 160, 250}, {"reader@lid", 0, -1},
      {"other@s.whatsapp.net", 0, 300}, {"empty", 0, 0}}, "Keep the uncertainty note."};
    MessageCache::MergeMessageReceiptAliases(info,
      {{"123@lid", "447700900123@s.whatsapp.net"},
       {"447700900123:2@s.whatsapp.net", "447700900123@s.whatsapp.net"}});
    assert(info.receipts.size() == 3 && info.note == "Keep the uncertainty note.");
    assert(info.receipts[0].userId == "447700900123@s.whatsapp.net" &&
      info.receipts[0].deliveredTime == 150 && info.receipts[0].readTime == 200);
    assert(info.receipts[1].userId == "other@s.whatsapp.net" &&
      info.receipts[1].deliveredTime == 0 && info.receipts[1].readTime == 300);
    assert(info.receipts[2].userId == "reader@lid" && info.receipts[2].readTime == -1);
    MessageCache::MergeMessageReceiptAliases(info, {});
    assert(info.receipts.size() == 3); // Idempotent; no fabricated recipients or dates.
    writer << "SELECT count(*) FROM messages WHERE chatId='chat' AND isRead=0;" >> count;
    assert(count == 1);
  }
  {
    writer << "INSERT INTO messages(chatId,id,timeSent,isOutgoing,isRead) VALUES"
      "('anchors','old',1000,0,1),('anchors','same-time',2000,0,0),"
      "('anchors','next-b',3000,0,1),('anchors','next-a',3000,1,1),('other','wrong-chat',2500,0,0),"
      "('anchors','',2100,0,0),('anchors','+local',2200,0,0);";
    ChatMessage anchor;
    assert(MessageCache::FindNewerMessage("test", "anchors", 2000, anchor));
    assert(anchor.id == "next-a" && anchor.timeSent == 3000 && anchor.isOutgoing);
    assert(!MessageCache::FindNewerMessage("test", "anchors", 3000, anchor));
    assert(anchor.id.empty());
    assert(!MessageCache::FindNewerMessage("test", "anchors' OR 1=1 --", 2000, anchor));
    assert(!MessageCache::FindNewerMessage("missing", "anchors", 2000, anchor));
    assert(!MessageCache::FindNewerMessage("test", "anchors", 0, anchor));
    for (const auto& item : {std::make_pair("old", true), std::make_pair("same-time", false)})
    {
      ChatMessage historical;
      historical.id = item.first;
      historical.timeSent = 99999;
      historical.isRead = !item.second; // Historical inference must never flip either known state.
      MessageCache::PrepareHistoricalMessage("test", "anchors", historical);
      assert(historical.isRead == item.second && historical.timeSent != 99999);
    }
    ChatMessage missing;
    missing.id = "not-yet-cached";
    missing.isOutgoing = false;
    missing.isRead = false;
    missing.timeSent = 100;
    MessageCache::PrepareHistoricalMessage("test", "anchors", missing);
    assert(missing.isRead && missing.timeSent == 100); // Local non-notifying history only.
    ChatMessage outgoing;
    outgoing.id = "outgoing-not-cached";
    outgoing.isOutgoing = true;
    outgoing.isRead = false;
    outgoing.timeSent = 100;
    MessageCache::PrepareHistoricalMessage("test", "anchors", outgoing);
    assert(!outgoing.isRead); // No invented checkmark/read confirmation for our old outgoing message.
    writer << "SELECT count(*) FROM messages WHERE chatId='anchors' AND id='same-time' AND isRead=0;" >> count;
    assert(count == 1); // Preparation itself is read-only and cannot send provider requests.
    ChatMessage stale;
    stale.id = "same-time";
    stale.isOutgoing = false;
    stale.timeSent = 99999;
    stale.fileInfo = "obsolete-file";
    MessageCache::PrepareHistoricalMessage("test", "anchors", stale);
    assert(!stale.isRead && stale.timeSent == 2000);
    writer << "UPDATE messages SET isRead=1,fileInfo='fresh-file' WHERE chatId='anchors' AND id='same-time';";
    auto history = std::make_shared<MessageCache::AddMessagesRequest>();
    history->profileId = "test"; history->chatId = "anchors";
    history->historical = true; history->chatMessages = {stale};
    MessageCache::PerformRequest(history);
    std::vector<ChatMessage> stored;
    assert(MessageCache::GetOneMessage("test", "anchors", "same-time", stored));
    assert(stored.front().isRead && stored.front().timeSent == 2000 && stored.front().fileInfo == "fresh-file");
    history->chatMessages = stored;
    writer << "UPDATE messages SET isRead=0,fileInfo='newer-file' WHERE chatId='anchors' AND id='same-time';";
    MessageCache::PerformRequest(history);
    stored.clear();
    assert(MessageCache::GetOneMessage("test", "anchors", "same-time", stored));
    assert(!stored.front().isRead && stored.front().timeSent == 2000 && stored.front().fileInfo == "newer-file");
    history->chatMessages = {missing, outgoing};
    MessageCache::PerformRequest(history);
    stored.clear();
    assert(MessageCache::GetOneMessage("test", "anchors", missing.id, stored) && stored.front().isRead);
    stored.clear();
    assert(MessageCache::GetOneMessage("test", "anchors", outgoing.id, stored) && !stored.front().isRead);
    ChatMessage recoveredFile;
    recoveredFile.id = "old"; recoveredFile.isOutgoing = false;
    recoveredFile.fileInfo = "recovered-attachment-metadata";
    MessageCache::PrepareHistoricalMessage("test", "anchors", recoveredFile);
    assert(recoveredFile.fileInfo == "recovered-attachment-metadata");
    history->chatMessages = {recoveredFile};
    MessageCache::PerformRequest(history);
    stored.clear();
    assert(MessageCache::GetOneMessage("test", "anchors", recoveredFile.id, stored));
    assert(stored.front().fileInfo == recoveredFile.fileInfo && stored.front().isRead);
    writer << "DELETE FROM messages WHERE chatId='anchors' OR id='wrong-chat';";
  }
  std::cout << "production receipt recovery: verified alias deduplication and exact newer history anchor passed\n";
  {
    sqlite::database reader(std::string(directory) + "/history/test/db.sqlite");
    reader << "BEGIN;";
    reader << "SELECT count(*) FROM messages;" >> count;
    std::atomic<bool> started{false}, complete{false};
    std::exception_ptr failure;
    std::thread update([&] {
      started = true;
      try {
        writer << "BEGIN;";
        writer << "UPDATE messages SET isRead=1;";
        writer << "COMMIT;";
      } catch (...) { failure = std::current_exception(); }
      complete = true;
    });
    while (!started) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(!complete); // The writer waits rather than throwing SQLITE_BUSY.
    reader << "COMMIT;";
    update.join();
    if (failure) std::rethrow_exception(failure);
    writer << "SELECT count(*) FROM messages WHERE isRead=1;" >> count;
    assert(count == 1);
  }
  MessageCache::Cleanup();
  AppConfig::Cleanup();
  FileUtil::RmDir(directory);
  std::cout << "production cache: concurrent reader and writer commit wait/recovery passed\n";
}
catch (const sqlite::sqlite_exception& error)
{
  std::cerr << error.what() << ": " << error.get_sql() << '\n';
  return 1;
}
