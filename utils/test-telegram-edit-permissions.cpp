// Real Telegram permission callbacks with synthetic replies; never logs in or edits.
#include "../lib/tgchat/src/tgchat.cpp"
#include <cassert>
#include <iostream>

using namespace td::td_api;
struct TelegramMuteHandlerTest
{
  static void Run()
  {
    for (bool caption : {false, true}) for (bool allowed : {false, true})
    {
      TgChat::Impl impl;
      assert(impl.HasFeature(FeatureEditMessagePermissions));
      assert(!impl.HasFeature(FeatureEditMessagesWithinTwoDays));
      int replies = 0; bool editable = !allowed;
      auto request = std::make_shared<GetMessageEditabilityRequest>();
      request->chatId = StrUtil::NumToHex(int64_t(1001)); request->msgId = StrUtil::NumToHex(int64_t(10));
      request->onResult = [&](bool ok, std::string reason) { ++replies; editable = ok; assert(ok || !reason.empty()); };
      impl.PerformRequest(request);
      assert(!impl.m_ClientManager && impl.m_Handlers.size() == 1);
      auto message = make_object<td::td_api::message>();
      message->date_ = 1; // Decades old: permissions, not age, decide editability.
      message->content_ = caption ? object_ptr<MessageContent>(make_object<messagePhoto>()) :
        object_ptr<MessageContent>(make_object<messageText>());
      impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(message)});
      assert(replies == 0 && impl.m_Handlers.size() == 1);
      auto properties = make_object<messageProperties>(); properties->can_be_edited_ = allowed;
      impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(properties)});
      assert(replies == 1 && editable == allowed && impl.m_Handlers.empty());
      assert(impl.m_CurrentQueryId == 2); // Load + permission; no edit/view/read query.
    }
    for (int failure : {0, 1, 2})
    {
      TgChat::Impl impl;
      int replies = 0;
      auto request = std::make_shared<GetMessageEditabilityRequest>();
      request->chatId = "1"; request->msgId = "2";
      request->onResult = [&](bool ok, std::string reason) { ++replies; assert(!ok && !reason.empty()); };
      impl.PerformRequest(request);
      if (failure == 0)
        impl.ProcessResponse({0, impl.m_Handlers.begin()->first, make_object<error>(404, "Synthetic missing message")});
      else
      {
        auto message = make_object<td::td_api::message>();
        if (failure == 1) message->content_ = make_object<messagePoll>();
        else message->content_ = make_object<messageText>();
        impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(message)});
        if (failure == 2)
          impl.ProcessResponse({0, impl.m_Handlers.begin()->first, make_object<error>(500, "Synthetic properties failure")});
      }
      assert(replies == 1 && impl.m_Handlers.empty());
    }
    std::cout << "PASS: Telegram edit permission follows can_be_edited for old text/captions, rejects denied/missing/unsupported content and errors; no edits or read queries\n";
  }
};

int main(int argc, char** argv)
{
  assert(argc == 2);
  FileUtil::SetApplicationDir(argv[1]); AppConfig::Init();
  TelegramMuteHandlerTest::Run(); AppConfig::Cleanup();
}
