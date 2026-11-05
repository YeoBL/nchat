// Real WhatsApp contact bridge and cache; no accounts or network are opened.
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "protocol.h"
#include "config.h"
#include "sysutil.h"
#include <sqlite_modern_cpp.h>
#define private public
#include "wmchat.h"
#include "messagecache.h"
#undef private
#include <cassert>
#include <cstring>
#include <dlfcn.h>
#include <iostream>

int main()
{
  const std::string profile = "WhatsAppMd_offline_names";
  auto& db = MessageCache::m_Dbs[profile];
  db = std::make_unique<sqlite::database>(":memory:");
  *db << "CREATE TABLE contacts2(id TEXT UNIQUE ON CONFLICT REPLACE, name TEXT, phone TEXT, isSelf INT, isAlias INT);";
  *db << "INSERT INTO contacts2 VALUES('older@g.us','Earlier Group','',0,0),"
         "('person@s.whatsapp.net','Saved Person','15550002',0,0);";
  // Match nchat's local plugin loading: globally linking the Go SQLite copy
  // would interpose on the application's separate native SQLite library.
  void* library = dlopen("./build/lib/libwmchat.dylib", RTLD_NOW | RTLD_LOCAL);
  assert(library);
  auto create = reinterpret_cast<WmChat*(*)()>(dlsym(library, "CreateWmChat"));
  auto add = reinterpret_cast<void(*)(int, WmChat*)>(dlsym(library, "_ZN6WmChat11AddInstanceEiPS_"));
  auto remove = reinterpret_cast<void(*)(int)>(dlsym(library, "_ZN6WmChat14RemoveInstanceEi"));
  auto notify = reinterpret_cast<decltype(&WmNewContactsNotify)>(dlsym(library, "WmNewContactsNotify"));
  auto statusNotify = reinterpret_cast<decltype(&WmNewStatusNotify)>(dlsym(library, "WmNewStatusNotify"));
  assert(create && add && remove && notify && statusNotify);
  std::unique_ptr<WmChat> provider(create());
  add(-801, provider.get());
  auto batch = [&](const std::string& name)
  {
    // Simulate a refresh whose group request failed, and an offline startup batch.
    notify(-801, strdup("person@s.whatsapp.net"), strdup(name.c_str()), strdup("15550002"), 0, 0, 1);
    notify(-801, strdup("alias@lid"), strdup("Saved Alias"), strdup("15550003"), 0, 1, 2);
    assert(provider->m_RequestsQueue.size() == 1);
    auto deferred = std::static_pointer_cast<DeferNotifyRequest>(provider->m_RequestsQueue.front());
    provider->m_RequestsQueue.pop_front();
    auto event = std::static_pointer_cast<NewContactsNotify>(deferred->serviceMessage);
    assert(!event->fullSync && event->contactInfos.size() == 2);
    assert(provider->m_ContactInfos.empty());
    auto request = std::make_shared<MessageCache::AddContactsRequest>();
    request->profileId = profile;
    request->fullSync = event->fullSync;
    request->contactInfos = event->contactInfos;
    MessageCache::PerformRequest(request);
  };
  batch("Renamed Person");
  batch("Renamed Again");
  std::string name;
  *db << "SELECT name FROM contacts2 WHERE id='older@g.us';" >> name;
  assert(name == "Earlier Group");
  *db << "SELECT name FROM contacts2 WHERE id='person@s.whatsapp.net';" >> name;
  assert(name == "Renamed Again");
  int alias = 0, count = 0;
  *db << "SELECT isAlias FROM contacts2 WHERE id='alias@lid';" >> alias;
  *db << "SELECT count(*) FROM contacts2;" >> count;
  assert(alias == 1 && count == 3);
  MessageCache::m_MessageHandler = [&](std::shared_ptr<ServiceMessage> message)
  {
    auto fetched = std::static_pointer_cast<NewContactsNotify>(message);
    assert(fetched->contactInfos.size() == 3);
    count = 0;
    for (const auto& contact : fetched->contactInfos)
      if (contact.id == "older@g.us" && contact.name == "Earlier Group") ++count;
    assert(count == 1);
  };
  auto fetch = std::make_shared<MessageCache::FetchContactsRequest>();
  fetch->profileId = profile;
  MessageCache::PerformRequest(fetch);
  MessageCache::m_MessageHandler = nullptr;
  for (const auto seconds : {1793872800, 0})
  {
    statusNotify(-801, strdup("person@s.whatsapp.net"), 0, seconds);
    assert(provider->m_RequestsQueue.size() == 1);
    auto deferred = std::static_pointer_cast<DeferNotifyRequest>(provider->m_RequestsQueue.front());
    provider->m_RequestsQueue.pop_front();
    auto event = std::static_pointer_cast<ReceiveStatusNotify>(deferred->serviceMessage);
    assert(event->userId == "person@s.whatsapp.net" && !event->isOnline);
    assert(event->timeSeen == (seconds ? 1793872800000LL : TimeSeenNone));
  }
  remove(-801);
  MessageCache::m_Dbs.clear();
  std::cout << "WhatsApp contact bridge: partial refresh preserves group names, updates names/aliases, and cache reload retains them\n";
  std::cout << "WhatsApp status bridge: exact seconds-to-milliseconds conversion and unknown timestamp sentinel passed\n";
}
