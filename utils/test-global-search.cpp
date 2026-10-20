// Uses in-memory production cache instances only; never opens a user profile.
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
#include "protocolutil.h"
#include <cassert>
#include <iostream>

int main()
{
  for (const auto& profile : {"Telegram_test", "WhatsApp_test"})
  {
    auto& db = MessageCache::m_Dbs[profile];
    db.reset(new sqlite::database(":memory:"));
    *db << "CREATE TABLE messages(chatId TEXT,id TEXT,senderId TEXT,text TEXT,quotedId TEXT,quotedText TEXT,"
      "quotedSender TEXT,fileInfo TEXT,timeSent INT,isOutgoing INT,isRead INT,isEdited INT,isDeleted INT,isPinned INT,"
      "UNIQUE(chatId,id) ON CONFLICT REPLACE);";
    *db << "CREATE TABLE contacts2(id TEXT PRIMARY KEY,name TEXT,phone TEXT,isSelf INT);";
    *db << "INSERT INTO contacts2 VALUES('alice','Alice','+44123',0),('me','Account owner','+44999',1),"
      "('group','College group','',0),('emilie','Émilie','+33123',0);";
    MessageCache::InitSearch(profile);
  }
  auto& tg = *MessageCache::m_Dbs.at("Telegram_test");
  auto& wa = *MessageCache::m_Dbs.at("WhatsApp_test");
  const auto insert = [](sqlite::database& db, const std::string& id, const std::string& sender,
    const std::string& text, int64_t time, const std::string& file = "", bool deleted = false)
  {
    db << "INSERT INTO messages(chatId,id,senderId,text,timeSent,fileInfo,isOutgoing,isRead,isDeleted) "
      "VALUES('group',?,?,?,?,?,?,0,?);" << id << sender << text << time << file << (sender == "me") << deleted;
  };
  FileInfo image;
  image.fileType = "image/jpeg"; image.filePath = "birthday.jpg";
  FileInfo audio;
  audio.fileType = "audio/ogg"; audio.filePath = "note.ogg";
  insert(tg, "1", "alice", "Birthday plans 🎂 and https://example.com/a", 100);
  insert(wa, "2", "alice", "A birthday photo", 300, ProtocolUtil::FileInfoToHex(image));
  insert(wa, "3", "me", "[Poll]\nBirthday dinner?", 200);
  insert(tg, "4", "alice", "[Voice note]", 400, ProtocolUtil::FileInfoToHex(audio));
  insert(tg, "5", "alice", "deleted birthday", 500, "", true);
  insert(tg, "6", "alice", "Exact \"quoted\" text and 你好", 600);
  insert(tg, "7", "emilie", "École", 700);
  FileInfo oldPhoto, oldVideo, oldAudio;
  oldPhoto.filePath = "[Photo]"; oldVideo.filePath = "downloaded.mp4"; oldAudio.filePath = "[VoiceNote]";
  insert(tg, "8", "alice", "Historic photo", 800, ProtocolUtil::FileInfoToHex(oldPhoto));
  insert(tg, "9", "alice", "Historic video caption", 900, ProtocolUtil::FileInfoToHex(oldVideo));
  insert(tg, "10", "alice", "Historic audio", 1000, ProtocolUtil::FileInfoToHex(oldAudio));

  MessageCache::SearchQuery query;
  query.text = "birthday";
  auto found = MessageCache::SearchMessages(query);
  assert(found.size() == 3 && found[0].message.id == "2" && found[2].message.id == "1");
  assert(found[0].chatName == "College group" && found[0].senderName == "Alice");
  query.sender = "You";
  found = MessageCache::SearchMessages(query);
  assert(found.size() == 1 && found[0].message.id == "3" && found[0].senderName == "You");
  query.sender = "+44123"; query.fromTime = 200; query.toTime = 350;
  found = MessageCache::SearchMessages(query);
  assert(found.size() == 1 && found[0].message.id == "2");
  query = {}; query.type = "image";
  assert(MessageCache::SearchMessages(query).size() == 2);
  query.type = "audio";
  assert(MessageCache::SearchMessages(query).size() == 2);
  query.type = "video";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "9");
  query.type = "file";
  assert(MessageCache::SearchMessages(query).empty());
  query.type = "poll";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "3");
  query.type = "DELETED";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "5");
  query = {}; query.linksOnly = true;
  assert(MessageCache::SearchMessages(query).at(0).message.id == "1");
  query = {}; query.text = "\"quoted\"";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "6");
  query.text = "你好";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "6");
  query.text = "école";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "7");
  query.text = "éc";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "7");
  query = {}; query.sender = "émilie";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "7");
  query.sender.clear();
  query.text = "' OR 1=1 --";
  assert(MessageCache::SearchMessages(query).empty());
  query = {}; query.profileId = "missing";
  assert(MessageCache::SearchMessages(query).empty());
  query = {}; query.chatId = "' OR 1=1 --";
  assert(MessageCache::SearchMessages(query).empty());
  query = {}; query.limit = 2;
  assert(MessageCache::SearchMessages(query).size() == 2);

  // Production replacement policy must update the FTS index without stale matches.
  insert(wa, "2", "alice", "Replacement caption", 301, ProtocolUtil::FileInfoToHex(image));
  query = {}; query.text = "birthday";
  assert(MessageCache::SearchMessages(query).size() == 2);
  query.text = "replacement";
  assert(MessageCache::SearchMessages(query).size() == 1);
  wa << "UPDATE messages SET text='An edited message' WHERE id='2';";
  assert(MessageCache::SearchMessages(query).empty());
  query.text = "edited";
  assert(MessageCache::SearchMessages(query).size() == 1);
  wa << "DELETE FROM messages WHERE id='2';";
  assert(MessageCache::SearchMessages(query).empty());
  MessageCache::m_SearchIndexed["Telegram_test"] = false;
  query.text = "école";
  assert(MessageCache::SearchMessages(query).at(0).message.id == "7");
  query.text = "birthday";
  assert(MessageCache::SearchMessages(query).size() == 2); // Same results without FTS5.
  query.fromTime = 1000; query.toTime = 1;
  assert(MessageCache::SearchMessages(query).empty());
  MessageCache::m_Dbs.clear();
  std::cout << "global cached search, bound filters, literal text, index updates and fallback: passed\n";
}
