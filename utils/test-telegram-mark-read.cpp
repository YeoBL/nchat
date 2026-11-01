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
    for (int scenario = 0; scenario < 5; ++scenario)
    {
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
        if (scenario == 4)
          message->topic_id_ = td::td_api::make_object<td::td_api::messageTopicForum>(7);
        respond(std::move(message));
        if (scenario != 1)
        {
          assert(results == before && notifications == 0 && impl.m_Handlers.size() == 1);
          if (scenario == 2)
            respond(td::td_api::make_object<td::td_api::error>(500, "Synthetic read failure"));
          else
            respond(td::td_api::make_object<td::td_api::ok>());
        }
      }
      assert(results == before + 1 && accepted == (scenario >= 3));
      assert(notifications == 0 && impl.m_Handlers.empty() && !impl.m_ClientManager);
    }
    // Only a real TDLib topic inbox update retires the restored forum target.
    impl.UpdateForumReadInbox(-1001, 7, 10485760);
    assert(notifications == 1);
    std::cout << "PASS: exact-message preload precedes read; missing/wrong targets and read errors fail; ordinary/forum acknowledgements never fake local read notifications\n";
  }
};

int main() { TelegramMuteHandlerTest::Run(); }
