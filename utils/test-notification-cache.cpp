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
    *files << "CREATE TABLE messages(chatId TEXT, id TEXT, isDeleted INT, fileInfo TEXT);";
    *files << "INSERT INTO messages VALUES ('chat','redacted',1,''),('chat','redacted-null',1,NULL),"
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
  MessageCache::m_Dbs.clear();
  std::cout << "production cache: late downloads preserve deletion redaction, retained media, and chat/account isolation\n";

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
