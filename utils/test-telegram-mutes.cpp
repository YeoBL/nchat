// Offline production-handler regression: no ClientManager, login, network,
// account configuration or real message database is created.
#include "../lib/tgchat/src/tgchat.cpp"
#include <cassert>
#include <iostream>

using namespace td::td_api;

namespace
{
object_ptr<chatNotificationSettings> Settings(bool inherited, int seconds = 0)
{
  auto result = make_object<chatNotificationSettings>();
  result->use_default_mute_for_ = inherited;
  result->mute_for_ = seconds;
  return result;
}

object_ptr<scopeNotificationSettings> ScopeSettings(int seconds = 0)
{
  auto result = make_object<scopeNotificationSettings>();
  result->mute_for_ = seconds;
  return result;
}

object_ptr<chat> Chat(int64_t id, object_ptr<ChatType> type, bool inherited = true, int seconds = 0)
{
  auto result = make_object<chat>();
  result->id_ = id;
  result->title_ = "Synthetic chat";
  result->type_ = std::move(type);
  result->notification_settings_ = Settings(inherited, seconds);
  return result;
}

object_ptr<ChatType> Private() { return make_object<chatTypePrivate>(1001); }
object_ptr<ChatType> Secret() { return make_object<chatTypeSecret>(7, 1002); }
object_ptr<ChatType> Group() { return make_object<chatTypeBasicGroup>(1003); }
object_ptr<ChatType> Supergroup(bool channel = false) { return make_object<chatTypeSupergroup>(1004, channel); }

object_ptr<td::td_api::message> Message(const std::string& body, int64_t id = 10)
{
  auto message = make_object<td::td_api::message>();
  message->id_ = id;
  message->chat_id_ = 1001;
  message->sender_id_ = make_object<messageSenderUser>(1001);
  message->date_ = 1700000000;
  auto content = make_object<messageText>();
  content->text_ = make_object<formattedText>();
  content->text_->text_ = body;
  message->content_ = std::move(content);
  return message;
}

void CheckHelper()
{
  TelegramMuteState state;
  assert(state.IsMuted(99) && !state.IsResolved(99));
  auto privateChat = Chat(1, Private());
  auto secretChat = Chat(2, Secret());
  auto group = Chat(3, Group());
  auto supergroup = Chat(4, Supergroup());
  auto channel = Chat(5, Supergroup(true));
  for (auto* item : {privateChat.get(), secretChat.get(), group.get(), supergroup.get(), channel.get()})
    assert(state.SetChat(*item));
  assert(!state.IsResolved(1));
  assert(TelegramMuteState::GetScope(static_cast<const ChatType*>(nullptr)) == TelegramMuteState::Unknown);
  assert(TelegramMuteState::GetScope(static_cast<const NotificationSettingsScope*>(nullptr)) == TelegramMuteState::Unknown);
  const auto privateRequest = state.BeginScopeRequest(TelegramMuteState::Private);
  assert(privateRequest && !state.BeginScopeRequest(TelegramMuteState::Private));
  assert(state.SetScope(TelegramMuteState::Private, nullptr, privateRequest).empty());
  assert(state.IsMuted(1));
  const auto retry = state.BeginScopeRequest(TelegramMuteState::Private);
  assert(retry && retry != privateRequest);
  assert(state.SetScope(TelegramMuteState::Private, ScopeSettings().get(), privateRequest).empty());
  assert(state.IsMuted(1));
  assert(state.SetScope(TelegramMuteState::Private, ScopeSettings().get(), retry) ==
    TelegramMuteState::Changes({{1, false}, {2, false}}));
  assert(!state.IsMuted(1) && !state.IsMuted(2) && state.IsMuted(3) && state.IsMuted(5));
  assert(state.IsResolved(1) && !state.IsResolved(3));
  assert(state.SetScope(TelegramMuteState::Group, ScopeSettings().get()) ==
    TelegramMuteState::Changes({{3, false}, {4, false}}));
  assert(state.IsMuted(5));
  auto override = Settings(false, 60);
  assert(state.SetChatSettings(1, override.get()));
  assert(state.SetScope(TelegramMuteState::Private, ScopeSettings(60).get()) ==
    TelegramMuteState::Changes({{2, true}}));
  override = Settings(false);
  assert(!state.SetChatSettings(1, override.get()));
  assert(state.SetScope(TelegramMuteState::Private, ScopeSettings().get()) ==
    TelegramMuteState::Changes({{2, false}}));
  assert(!state.SetChatSettings(1, Settings(true, 999).get())); // inherited ignores stale override
  assert(state.SetScope(TelegramMuteState::Private, ScopeSettings(60).get()) ==
    TelegramMuteState::Changes({{1, true}, {2, true}}));
  assert(state.SetScope(TelegramMuteState::Channel, ScopeSettings().get()) ==
    TelegramMuteState::Changes({{5, false}}));
  assert(!state.SetChatSettings(6, Settings(false).get()));
  assert(!state.HasChatScope(6) && state.IsResolved(6));
  assert(state.SetChatSettings(6, Settings(true).get()));
  assert(!state.IsResolved(6));
  auto typed = Chat(6, Private());
  assert(state.SetChat(*typed));
  assert(state.HasChatScope(6));

  TelegramMuteState stale;
  auto pending = stale.BeginScopeRequest(TelegramMuteState::Group);
  stale.SetScope(TelegramMuteState::Group, ScopeSettings(60).get());
  assert(stale.SetScope(TelegramMuteState::Group, ScopeSettings().get(), pending).empty());
  assert(stale.SetChat(*Chat(20, Group())));
  assert(!stale.BeginScopeRequest(TelegramMuteState::Unknown));
  std::cout << "PASS: inherited/override, private/secret/group/channel, unknown, retry and stale-response mute state\n";
}
}

struct TelegramMuteHandlerTest
{
  static void CheckSearch()
  {
    TgChat::Impl impl;
    impl.m_Config.Set("markdown_enabled", "0");
    impl.m_Config.Set("markdown_version", "2");
    struct State { bool ready = false; ClientActionResult result; };
    auto run = [&](const std::string& operation, std::map<std::string, std::string> params)
    {
      auto state = std::make_shared<State>();
      auto request = std::make_shared<ClientActionRequest>();
      request->operation = operation;
      request->params = std::move(params);
      request->onResult = [state](ClientActionResult result)
        { state->ready = true; state->result = std::move(result); };
      impl.PerformClientAction(request);
      return state;
    };
    auto find = [](const ClientActionResult& result, const std::string& title)
    {
      for (const auto& item : result.items) if (item.title == title) return item;
      assert(false && "Missing Telegram search action");
      return ClientActionItem{};
    };
    const auto current = StrUtil::NumToHex(int64_t(1001));
    auto menu = run("menu", {{"chatId", current}, {"topicId", "7"}});
    assert(menu->ready);
    auto search = find(menu->result, "Search Telegram history");
    assert(search.operation == "tg.search.run" && search.fields.size() == 1);
    assert(search.fields.front().key == "query" && search.params.at("_quickForm") == "1");
    assert(!search.confirm && search.params.at("type") == "all");
    assert(search.params.at("sender").empty() && search.params.at("after").empty() &&
      search.params.at("before").empty() && search.params.at("topicId").empty());
    assert(search.params.at("searchChatId") == current && search.params.at("searchTopicId") == "7");
    search.params["query"] = "Syntheticneedle";
    auto results = run(search.operation, search.params);
    assert(!results->ready && impl.m_Handlers.size() == 1);
    auto page = make_object<foundMessages>();
    page->next_offset_ = "synthetic-next-page";
    const std::string body = "Syntheticneedle: full message body only once";
    page->messages_.push_back(Message(body));
    impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(page)});
    assert(results->ready && results->result.success);
    assert(results->result.text.find(body) == std::string::npos);
    int rendered = 0;
    for (const auto& item : results->result.items)
      if (item.operation == "tg.message") { assert(item.detail == body); ++rendered; }
    assert(rendered == 1);
    auto filters = find(results->result, "Search filters and scope");
    assert(filters.operation == "tg.search" && filters.fields.empty());
    auto next = find(results->result, "Older / more messages");
    assert(next.params.at("offset") == "synthetic-next-page");
    filters.params["type"] = "photo";
    filters.params["sender"] = "Synthetic Person";
    filters.params["senderResolved"] = "1001";
    filters.params["offset"] = "old-offset";
    filters.params["from"] = current;
    filters.params["after"] = "2026-01-01";
    filters.params["before"] = "2026-10-20";
    auto optional = run(filters.operation, filters.params);
    assert(optional->ready);
    auto sender = find(optional->result, "Sender (optional)");
    assert(sender.fields.size() == 1 && sender.fields.front().key == "sender");
    assert(sender.params.at("senderResolved").empty() && sender.params.at("offset").empty() &&
      sender.params.at("from").empty());
    assert(sender.params.at("query") == "Syntheticneedle" && sender.params.at("type") == "photo" &&
      sender.params.at("after") == "2026-01-01" && sender.params.at("before") == "2026-10-20");
    auto localScope = find(optional->result, "Scope: this chat or topic");
    assert(localScope.params.at("scope") == "chat" && localScope.params.at("chatId") == current &&
      localScope.params.at("topicId") == "7");
    auto typeMenu = find(optional->result, "Message type: photo");
    assert(typeMenu.fields.empty());
    auto types = run(typeMenu.operation, typeMenu.params);
    assert(types->ready && types->result.items.size() == 9);
    for (const auto& item : types->result.items)
    {
      assert(item.title != "pinned" && item.fields.empty());
      assert(item.params.at("query") == "Syntheticneedle" && item.params.at("offset").empty());
    }
    typeMenu.params["scope"] = "chat";
    auto chatTypes = run(typeMenu.operation, typeMenu.params);
    assert(chatTypes->ready && chatTypes->result.items.size() == 10);
    assert(find(chatTypes->result, "pinned").params.at("type") == "pinned");
    filters.params["type"] = "pinned";
    filters.params["scope"] = "chat";
    auto scopedFilters = run("tg.search", filters.params);
    const auto allScope = find(scopedFilters->result, "Scope: all Telegram chats");
    assert(allScope.params.at("scope").empty() && allScope.params.at("type") == "all");
    assert(!impl.m_ClientManager && impl.m_Handlers.empty());
    for (const std::string operation : {"tg.topic.history", "tg.thread.history", "tg.album"})
    {
      auto context = std::map<std::string, std::string>{{"chatId", current},
        {"msgId", StrUtil::NumToHex(int64_t(10))}};
      if (operation == "tg.topic.history") context["topicId"] = "7";
      if (operation == "tg.thread.history") context["threadId"] = "8";
      auto history = run(operation, context);
      assert(!history->ready && impl.m_Handlers.size() == 1);
      if (operation == "tg.album")
      {
        auto first = Message(body);
        first->media_album_id_ = 9;
        impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(first)});
        assert(!history->ready && impl.m_Handlers.size() == 1);
      }
      auto historyPage = make_object<messages>();
      auto fullMessage = Message(body);
      if (operation == "tg.album") fullMessage->media_album_id_ = 9;
      historyPage->messages_.push_back(std::move(fullMessage));
      impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(historyPage)});
      assert(history->ready && history->result.success && history->result.text.find(body) == std::string::npos);
      int copies = 0;
      for (const auto& item : history->result.items)
        if (item.operation == "tg.message") { assert(item.detail == body); ++copies; }
      assert(copies == 1);
      if (operation != "tg.album")
      {
        const auto reply = find(history->result, "Reply here");
        assert(reply.confirm && reply.params.at(operation == "tg.topic.history" ? "topicId" : "threadId") ==
          (operation == "tg.topic.history" ? "7" : "8"));
      }
    }
    std::cout << "PASS: production Telegram text-first search, optional filters/scope/type, pagination and nonduplicated result callbacks\n";
    std::cout << "PASS: production Telegram topic/comment/album pages preserve full selectable text without duplicate transcript\n";
  }

  static void Run()
  {
    TgChat::Impl impl;
    assert(!impl.m_ClientManager);
    std::vector<std::pair<int64_t, bool>> notices;
    std::vector<ChatInfo> chatInfos;
    impl.SetMessageHandler([&](std::shared_ptr<ServiceMessage> message)
    {
      if (message->GetMessageType() == UpdateMuteNotifyType)
      {
        const auto update = std::static_pointer_cast<UpdateMuteNotify>(message);
        assert(update->success);
        notices.emplace_back(StrUtil::NumFromHex<int64_t>(update->chatId), update->isMuted);
      }
      else if (message->GetMessageType() == NewChatsNotifyType)
      {
        const auto update = std::static_pointer_cast<NewChatsNotify>(message);
        chatInfos.insert(chatInfos.end(), update->chatInfos.begin(), update->chatInfos.end());
      }
    });
    auto pushChat = [&](object_ptr<chat> chat)
    {
      auto update = make_object<updateNewChat>();
      update->chat_ = std::move(chat);
      impl.ProcessUpdate(std::move(update));
    };
    auto pushScope = [&](object_ptr<NotificationSettingsScope> scope, int seconds)
    {
      auto update = make_object<updateScopeNotificationSettings>();
      update->scope_ = std::move(scope);
      update->notification_settings_ = ScopeSettings(seconds);
      impl.ProcessUpdate(std::move(update));
    };
    auto pushSettings = [&](int64_t id, bool inherited, int seconds)
    {
      auto update = make_object<updateChatNotificationSettings>();
      update->chat_id_ = id;
      update->notification_settings_ = Settings(inherited, seconds);
      impl.ProcessUpdate(std::move(update));
    };
    auto reply = [&](uint64_t id, object_ptr<Object> value)
    {
      assert(impl.m_Handlers.count(id));
      impl.ProcessResponse({0, id, std::move(value)});
    };
    pushChat(Chat(101, Private()));
    pushChat(Chat(102, Secret()));
    pushChat(Chat(103, Group()));
    pushChat(Chat(104, Supergroup(true)));
    assert(notices == TelegramMuteState::Changes({{101,true},{102,true},{103,true},{104,true}}));
    notices.clear();
    impl.RequestNotificationScopes();
    assert(impl.m_Handlers.size() == 3);
    // A pushed mute supersedes an older successful scope fetch.
    pushScope(make_object<notificationSettingsScopePrivateChats>(), 60);
    reply(1, ScopeSettings());
    assert(notices.empty() && impl.m_NotificationMutes.IsMuted(101));
    reply(2, make_object<error>(503, "Synthetic disconnected"));
    reply(3, ScopeSettings());
    assert(notices == TelegramMuteState::Changes({{104,false}}));
    notices.clear();
    impl.RequestNotificationScopes();
    assert(impl.m_Handlers.size() == 1);
    reply(4, ScopeSettings());
    assert(notices == TelegramMuteState::Changes({{103,false}}));
    notices.clear();
    pushSettings(101, false, 0);
    assert(notices == TelegramMuteState::Changes({{101,false}}));
    notices.clear();
    pushScope(make_object<notificationSettingsScopePrivateChats>(), 0);
    assert(notices == TelegramMuteState::Changes({{102,false}}));
    notices.clear();
    pushSettings(101, true, 999);
    assert(notices == TelegramMuteState::Changes({{101,false}}));
    notices.clear();
    pushScope(make_object<notificationSettingsScopePrivateChats>(), 60);
    assert(notices == TelegramMuteState::Changes({{101,true},{102,true}}));
    notices.clear();
    // A settings update can precede the chat type. It remains fail-closed and
    // queues authoritative getChat metadata rather than guessing a scope.
    pushSettings(105, true, 0);
    assert(notices == TelegramMuteState::Changes({{105,true}}));
    assert(!impl.m_NotificationMutes.HasChatScope(105));
    assert(!impl.m_RequestsQueue.empty());
    notices.clear();
    // Exercise the real startup/targeted getChat callback and resulting cache
    // notification, not just the standalone helper or source-string assertions.
    auto details = std::make_shared<DeferGetChatDetailsRequest>();
    details->chatIds = {StrUtil::NumToHex(int64_t(105))};
    impl.PerformRequest(details);
    const auto metadataRequest = impl.m_Handlers.rbegin()->first;
    reply(metadataRequest, Chat(105, Private()));
    assert(impl.m_NotificationMutes.HasChatScope(105));
    assert(chatInfos.size() == 1 && chatInfos.front().isMuted);
    pushScope(make_object<notificationSettingsScopePrivateChats>(), 0);
    assert(notices == TelegramMuteState::Changes({{101,false},{102,false},{105,false}}));
    assert(!impl.m_ClientManager);
    std::cout << "PASS: production Telegram scope/settings/new-chat handlers and startup metadata callbacks (no client/network)\n";
  }
};

int main(int argc, char** argv)
{
  assert(argc == 2);
  FileUtil::SetApplicationDir(argv[1]);
  AppConfig::Init();
  AppConfig::SetBool("cache_enabled", false);
  MessageCache::Init();
  CheckHelper();
  TelegramMuteHandlerTest::Run();
  TelegramMuteHandlerTest::CheckSearch();
  MessageCache::Cleanup();
  AppConfig::Cleanup();
}
