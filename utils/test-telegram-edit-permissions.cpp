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
    for (bool contentUpdate : {false, true})
    {
      TgChat::Impl impl;
      impl.m_Config.Set("markdown_enabled", "0");
      impl.m_Config.Set("markdown_version", "2");
      int delivered = 0;
      impl.SetMessageHandler([&](std::shared_ptr<ServiceMessage> event) {
        assert(event->GetMessageType() == NewMessagesNotifyType);
        auto notify = std::static_pointer_cast<NewMessagesNotify>(event);
        assert(notify->success && !notify->cached && !notify->sequence && notify->contentUpdate);
        assert(notify->chatId == StrUtil::NumToHex(int64_t(1001)));
        assert(notify->chatMessages.size() == 1);
        const auto& updated = notify->chatMessages.front();
        assert(updated.id == StrUtil::NumToHex(int64_t(10)));
        assert(updated.text == "Updated Unicode ✓" && updated.isEdited);
        ++delivered;
      });
      if (contentUpdate)
      {
        auto update = make_object<updateMessageContent>();
        update->chat_id_ = 1001; update->message_id_ = 10;
        impl.ProcessUpdate(std::move(update));
      }
      else
      {
        auto update = make_object<updateMessageEdited>();
        update->chat_id_ = 1001; update->message_id_ = 10;
        impl.ProcessUpdate(std::move(update));
      }
      assert(impl.m_RequestsQueue.size() == 1);
      auto request = std::static_pointer_cast<GetMessageRequest>(impl.m_RequestsQueue.front());
      assert(!request->cached);
      impl.PerformRequest(request);
      assert(impl.m_Handlers.size() == 1 && impl.m_CurrentQueryId == 1);
      auto message = make_object<td::td_api::message>();
      message->chat_id_ = 1001; message->id_ = 10; message->edit_date_ = 100;
      message->sender_id_ = make_object<messageSenderUser>(1002);
      auto text = make_object<messageText>();
      text->text_ = make_object<formattedText>(); text->text_->text_ = "Updated Unicode ✓";
      message->content_ = std::move(text);
      impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(message)});
      assert(delivered == 1 && impl.m_Handlers.empty());
    }
    {
      TgChat::Impl impl;
      impl.m_Config.Set("markdown_enabled", "0");
      impl.m_Config.Set("markdown_version", "2");
      auto request = std::make_shared<SetCurrentChatRequest>();
      request->chatId = StrUtil::NumToHex(int64_t(1001));
      impl.PerformRequest(request);
      assert(impl.m_CurrentChat == 1001 && impl.m_CurrentQueryId == 2);
      assert(impl.m_Handlers.size() == 2); // openChat + recent history, no viewMessages.
      impl.PerformRequest(request);
      assert(impl.m_CurrentQueryId == 2); // Same-chat status refreshes do not repeat history reads.
    }
    std::cout << "PASS: Telegram content/edit updates fetch and deliver the exact edited message without view/read queries\n";
    std::cout << "PASS: Telegram edit permission follows can_be_edited for old text/captions, rejects denied/missing/unsupported content and errors; no edits or read queries\n";
  }
};

int main(int argc, char** argv)
{
  assert(argc == 2);
  FileUtil::SetApplicationDir(argv[1]); AppConfig::Init();
  TelegramMuteHandlerTest::Run(); AppConfig::Cleanup();
}
