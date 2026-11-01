// Offline production-handler test: synthetic messages, no TDLib client/login.
#include "../lib/tgchat/src/tgchat.cpp"
#include <cassert>
#include <iostream>

struct TelegramMuteHandlerTest
{
  static void Run()
  {
    TgChat::Impl impl;
    int notifications = 0, results = 0;
    bool accepted = false;
    impl.SetMessageHandler([&](std::shared_ptr<ServiceMessage>) { ++notifications; });
    auto respond = [&](TgChat::Impl::Object value) {
      assert(impl.m_Handlers.size() == 1);
      auto handler = std::move(impl.m_Handlers.begin()->second);
      impl.m_Handlers.clear();
      handler(std::move(value));
    };
    for (int scenario = 0; scenario < 9; ++scenario)
    {
      notifications = 0;
      auto request = std::make_shared<MarkMessageReadRequest>();
      request->chatId = StrUtil::NumToHex(int64_t(-1001));
      request->msgId = StrUtil::NumToHex(int64_t(10485760));
      request->onResult = [&](bool success) { ++results; accepted = success; };
      const int before = results;
      impl.PerformRequest(request);
      assert(results == before && notifications == 0 && impl.m_Handlers.size() == 1);
      if (scenario == 0)
        respond(td::td_api::make_object<td::td_api::error>(404, "Synthetic missing message"));
      else
      {
        auto message = td::td_api::make_object<td::td_api::message>();
        message->chat_id_ = scenario == 1 ? -1002 : -1001;
        message->id_ = 10485760;
        if (scenario == 4 || scenario == 8)
          message->topic_id_ = td::td_api::make_object<td::td_api::messageTopicForum>(7);
        respond(std::move(message));
        if (scenario != 1)
        {
          assert(results == before && notifications == 0 && impl.m_Handlers.size() == 1);
          if (scenario == 2)
            respond(td::td_api::make_object<td::td_api::error>(500, "Synthetic read failure"));
          else
          {
            respond(td::td_api::make_object<td::td_api::ok>());
            assert(results == before && notifications == 0 && impl.m_Handlers.size() == 1);
            if (scenario == 4 || scenario == 8)
            {
              auto topic = td::td_api::make_object<td::td_api::forumTopic>();
              topic->info_ = td::td_api::make_object<td::td_api::forumTopicInfo>();
              topic->info_->chat_id_ = -1001;
              topic->info_->forum_topic_id_ = scenario == 8 ? 8 : 7;
              topic->last_read_inbox_message_id_ = 10485760;
              respond(std::move(topic));
            }
            else if (scenario == 7)
              respond(td::td_api::make_object<td::td_api::error>(500, "Synthetic marker failure"));
            else
            {
              auto chat = td::td_api::make_object<td::td_api::chat>();
              chat->id_ = -1001;
              chat->last_read_inbox_message_id_ = scenario == 5 ? 1 : 10485760;
              chat->is_marked_as_unread_ = scenario == 6;
              respond(std::move(chat));
            }
          }
        }
      }
      const bool confirmed = scenario == 3 || scenario == 4;
      assert(results == before + 1 && accepted == confirmed);
      assert((notifications > 0) == confirmed && impl.m_Handlers.empty() && !impl.m_ClientManager);
    }
    std::cout << "PASS: preload/read/marker ordering; ordinary and already-read forum snapshots reconcile; missing, wrong-topic, unread, marked-unread and failed snapshots never clear notifications\n";
  }
};

int main() { TelegramMuteHandlerTest::Run(); }
