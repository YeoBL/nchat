// Telegram account tools. Included here so requests use the existing TDLib
// session, response dispatcher and contact cache.
namespace
{
std::string TgFeatureParam(const std::map<std::string, std::string>& params, const std::string& key)
{
  const auto found = params.find(key);
  return found == params.end() ? "" : found->second;
}

ClientActionItem TgFeatureItem(const std::string& title, const std::string& operation,
  const std::map<std::string, std::string>& params = {},
  const std::vector<ClientActionField>& fields = {}, bool confirm = false, const std::string& detail = "")
{
  ClientActionItem item;
  item.title = title; item.operation = operation; item.params = params;
  item.fields = fields; item.confirm = confirm; item.detail = detail;
  return item;
}

bool TgFeatureError(const std::shared_ptr<ClientActionRequest>& request,
                    const td::td_api::object_ptr<td::td_api::Object>& object)
{
  if (object && object->get_id() != td::td_api::error::ID) return false;
  ClientActionResult result;
  result.success = false;
  result.title = "Telegram";
  result.text = object ? static_cast<const td::td_api::error&>(*object).message_ : "Telegram returned no response.";
  if (request->onResult) request->onResult(std::move(result));
  return true;
}

std::string TgFeatureTimestamp(int32_t timestamp)
{
  const std::time_t value = timestamp;
  std::tm date{};
  localtime_r(&value, &date);
  std::ostringstream stream;
  stream << std::put_time(&date, "%d %b %H:%M");
  return stream.str();
}

td::td_api::object_ptr<td::td_api::SearchMessagesFilter> TgFeatureFilter(const std::string& name)
{
  using namespace td::td_api;
  if (name == "photo") return make_object<searchMessagesFilterPhoto>();
  if (name == "video") return make_object<searchMessagesFilterVideo>();
  if (name == "audio") return make_object<searchMessagesFilterAudio>();
  if (name == "file") return make_object<searchMessagesFilterDocument>();
  if (name == "voice") return make_object<searchMessagesFilterVoiceNote>();
  if (name == "video note") return make_object<searchMessagesFilterVideoNote>();
  if (name == "link") return make_object<searchMessagesFilterUrl>();
  if (name == "poll") return make_object<searchMessagesFilterPoll>();
  if (name == "pinned") return make_object<searchMessagesFilterPinned>();
  return make_object<searchMessagesFilterEmpty>();
}
}

void TgChat::Impl::PerformClientAction(std::shared_ptr<ClientActionRequest> request)
{
  if (!request->onResult) return;
  using namespace td::td_api;
  const std::string operation = request->operation;
  auto params = request->params;
  params["profile"] = m_ProfileId;
  const auto chat = StrUtil::NumFromHex<int64_t>(TgFeatureParam(params, "chatId"));
  const auto message = StrUtil::NumFromHex<int64_t>(TgFeatureParam(params, "msgId"));
  auto finish = [request](const std::string& title, const std::string& text, bool success = true)
  {
    ClientActionResult result;
    result.title = title; result.text = text; result.success = success;
    request->onResult(std::move(result));
  };

  if (operation == "menu" || operation == "tg.menu.chats" || operation == "tg.menu.send" ||
      operation == "tg.menu.message")
  {
    ClientActionResult result;
    result.title = operation == "menu" ? "Telegram tools" : operation == "tg.menu.chats" ? "Chats and topics" :
      operation == "tg.menu.send" ? "Create and send" : "Selected message";
    auto add = [&](const std::string& title, const std::string& op,
      const std::vector<ClientActionField>& fields = {}, bool confirm = false, const std::string& detail = "")
      { result.items.push_back(TgFeatureItem(title, op, params, fields, confirm, detail)); };
    if (operation == "menu")
    {
      add("Search Telegram history", "tg.search");
      add("Chats, topics and Secret Chats", "tg.menu.chats");
      add("Stories", "tg.stories.list");
      if (chat)
      {
        add("Create, schedule or send", "tg.menu.send");
        add("Chat controls and administration", "tg.manage");
      }
      if (chat && message) add("Selected message actions", "tg.menu.message");
    }
    else if (operation == "tg.menu.chats")
    {
      if (chat)
      {
        add("Chat information and management", "tg.chat.info");
        if (m_ChatTypes[chat] == ChatSuperGroup) add("Topics in this group", "tg.topics");
        add(m_ChatTypes[chat] == ChatSecret ? "Draft on this device" : "Cloud draft", "tg.draft.get");
        add("Stories from this chat", "tg.stories.chat");
      }
      add("Secret Chats on this device", "tg.secret.list");
      add("Start a Secret Chat", "tg.secret.new",
        {{"contact", "Contact @username or Telegram user ID (blank: selected private chat)", ""}}, true);
    }
    else if (operation == "tg.menu.send")
    {
      add("Send silently", "tg.send.silent", {{"sendText", "Message", TgFeatureParam(params, "draft")}}, true);
      add("Schedule a message", "tg.send.scheduled",
        {{"sendText", "Message", TgFeatureParam(params, "draft")}, {"time", "Local time (YYYY-MM-DD HH:MM)", ""}}, true);
      add("Create a poll or quiz", "tg.poll.create",
        {{"question", "Question", ""}, {"options", "Options, separated by |", ""},
         {"multi", "Allow multiple choices (yes/no)", "no"}, {"anonymous", "Anonymous votes (yes/no)", "yes"},
         {"quiz", "Quiz (yes/no)", "no"},
         {"correct", "Correct answer number for quiz", "1"}}, true);
      add("Send a contact", "tg.contact.send",
        {{"first", "First name", ""}, {"last", "Last name", ""}, {"phone", "Full phone number", ""}}, true);
      add("Send a location", "tg.location.send",
        {{"latitude", "Latitude", ""}, {"longitude", "Longitude", ""}}, true);
      auto storyContext = params;
      storyContext["path"] = ""; storyContext["caption"] = "";
      storyContext["privacy"] = ""; storyContext["duration"] = "";
      result.items.push_back(TgFeatureItem("Post a story", "tg.story.post", storyContext));
    }
    else
    {
      add("Full message and attachments", "tg.message");
      add("Comments / discussion thread", "tg.comments");
      add("Show album", "tg.album");
      add("Translate", "tg.translate", {{"language", "Target language code", "en"}});
      add("Bot buttons", "tg.buttons");
      add("Message link", "tg.message.link");
      add("Forward with original attribution", "tg.forward.pick");
    }
    request->onResult(std::move(result));
    return;
  }

  if (HandleSocialAction(request) || HandleManageAction(request)) return;

  // Display full messages and keep topic/thread routing alongside every item.
  auto showMessages = [this, request](std::vector<object_ptr<td::td_api::message>> messages,
    const std::string& title, const std::map<std::string, std::string>& context,
    const std::string& nextOperation, const std::map<std::string, std::string>& nextParams)
  {
    ClientActionResult result;
    result.title = title;
    for (auto& value : messages)
    {
      if (!value) continue;
      auto itemParams = context;
      itemParams["chatId"] = StrUtil::NumToHex(value->chat_id_);
      itemParams["msgId"] = StrUtil::NumToHex(value->id_);
      itemParams["topicId"] = ""; itemParams["threadId"] = "";
      if (value->topic_id_ && value->topic_id_->get_id() == messageTopicForum::ID)
        itemParams["topicId"] = std::to_string(static_cast<messageTopicForum&>(*value->topic_id_).forum_topic_id_);
      if (value->topic_id_ && value->topic_id_->get_id() == messageTopicThread::ID)
        itemParams["threadId"] = std::to_string(static_cast<messageTopicThread&>(*value->topic_id_).message_thread_id_);
      ChatMessage converted;
      const auto date = value->date_;
      TdMessageConvert(*value, converted);
      const auto heading = GetContactName(StrUtil::NumFromHex<int64_t>(converted.senderId)) +
        " · " + TgFeatureTimestamp(date);
      result.text += heading + "\n" + converted.text + "\n\n";
      result.items.push_back(TgFeatureItem(heading, "tg.message", itemParams, {}, false, converted.text));
    }
    if (messages.empty()) result.text = "No messages on this page.";
    if (TgFeatureParam(context, "topicId") != "" || TgFeatureParam(context, "threadId") != "")
    {
      result.items.insert(result.items.begin(), TgFeatureItem("Reply here", "tg.thread.send", context,
        {{"text", "Message", ""}}, true));
      result.items.push_back(TgFeatureItem("Cloud draft for this topic", "tg.draft.get", context));
    }
    if (!nextOperation.empty()) result.items.push_back(TgFeatureItem("Older / more messages", nextOperation, nextParams));
    request->onResult(std::move(result));
  };

  if (operation == "tg.topics")
  {
    if (!chat) { finish("Topics", "Select a Telegram forum group first.", false); return; }
    auto query = make_object<getForumTopics>();
    query->chat_id_ = chat;
    query->query_ = TgFeatureParam(params, "query");
    query->offset_date_ = std::atoi(TgFeatureParam(params, "offsetDate").c_str());
    query->offset_message_id_ = StrUtil::NumFromHex<int64_t>(TgFeatureParam(params, "offsetMessage"));
    query->offset_forum_topic_id_ = std::atoi(TgFeatureParam(params, "offsetTopic").c_str());
    query->limit_ = 50;
    SendQuery(std::move(query), [request, params](Object object)
    {
      if (TgFeatureError(request, object)) return;
      auto topics = td::move_tl_object_as<forumTopics>(object);
      ClientActionResult result;
      result.title = "Topics";
      auto search = params;
      search["offsetDate"] = ""; search["offsetMessage"] = ""; search["offsetTopic"] = "";
      result.items.push_back(TgFeatureItem("Find a topic", "tg.topics", search, {{"query", "Topic name", ""}}));
      for (const auto& topic : topics->topics_)
      {
        if (!topic || !topic->info_) continue;
        auto context = params;
        context["topicId"] = std::to_string(topic->info_->forum_topic_id_);
        context["topicName"] = topic->info_->name_;
        context["from"] = "";
        context["msgId"] = ""; context["threadId"] = "";
        result.items.push_back(TgFeatureItem(topic->info_->name_, "tg.topic.history", context, {}, false,
          std::to_string(topic->unread_count_) + " unread" + (topic->info_->is_closed_ ? " · closed" : "")));
      }
      if (topics->next_offset_forum_topic_id_)
      {
        auto next = params;
        next["offsetDate"] = std::to_string(topics->next_offset_date_);
        next["offsetMessage"] = StrUtil::NumToHex(topics->next_offset_message_id_);
        next["offsetTopic"] = std::to_string(topics->next_offset_forum_topic_id_);
        result.items.push_back(TgFeatureItem("More topics", "tg.topics", next));
      }
      if (topics->topics_.empty()) result.text = "No topics found. This chat must be a forum group.";
      request->onResult(std::move(result));
    });
    return;
  }

  if (operation == "tg.comments")
  {
    if (!chat || !message) { finish("Comments", "Select a channel post first.", false); return; }
    SendQuery(make_object<getMessageThread>(chat, message), [this, request, params](Object object)
    {
      if (TgFeatureError(request, object)) return;
      auto thread = td::move_tl_object_as<messageThreadInfo>(object);
      auto followup = std::make_shared<ClientActionRequest>(*request);
      followup->operation = "tg.thread.history";
      followup->params = params;
      followup->params["sourceChatId"] = TgFeatureParam(params, "chatId");
      followup->params["sourceMsgId"] = TgFeatureParam(params, "msgId");
      followup->params["chatId"] = StrUtil::NumToHex(thread->chat_id_);
      followup->params["threadId"] = std::to_string(thread->message_thread_id_);
      followup->params["topicId"] = "";
      followup->params["msgId"] = "";
      followup->params["from"] = "";
      PerformClientAction(followup);
    });
    return;
  }

  if (operation == "tg.topic.history" || operation == "tg.thread.history")
  {
    int64_t id = 0;
    const bool forum = operation == "tg.topic.history";
    if (!chat || !TgFeatureParseNumber(TgFeatureParam(params, forum ? "topicId" : "threadId"), id) || id <= 0 ||
        (forum && id > INT32_MAX))
    { finish("Messages", "Topic or thread is unavailable.", false); return; }
    auto handler = [showMessages, request, params, operation, forum](Object object)
    {
      if (TgFeatureError(request, object)) return;
      auto page = td::move_tl_object_as<td::td_api::messages>(object);
      auto next = params;
      for (auto it = page->messages_.rbegin(); it != page->messages_.rend(); ++it)
        if (*it) { next["from"] = StrUtil::NumToHex((*it)->id_); break; }
      const auto boundary = StrUtil::NumFromHex<int64_t>(TgFeatureParam(params, "from"));
      page->messages_.erase(std::remove_if(page->messages_.begin(), page->messages_.end(),
        [boundary](const auto& value) { return value && boundary && value->id_ == boundary; }), page->messages_.end());
      const bool more = !page->messages_.empty() && TgFeatureParam(next, "from") != TgFeatureParam(params, "from");
      showMessages(std::move(page->messages_), forum ? TgFeatureParam(params, "topicName") : "Channel comments",
        params, more ? operation : "", next);
    };
    const auto from = StrUtil::NumFromHex<int64_t>(TgFeatureParam(params, "from"));
    if (forum) SendQuery(make_object<getForumTopicHistory>(chat, static_cast<int32_t>(id), from, 0, from ? 21 : 20), handler);
    else SendQuery(make_object<getMessageThreadHistory>(chat, id, from, 0, from ? 21 : 20), handler);
    return;
  }

  if (operation == "tg.thread.send")
  {
    const auto text = TgFeatureParam(params, "text");
    if (!chat || text.empty()) { finish("Reply", "Enter a message.", false); return; }
    auto query = make_object<sendMessage>();
    query->chat_id_ = chat;
    int64_t id = 0;
    if (TgFeatureParseNumber(TgFeatureParam(params, "topicId"), id) && id > 0 && id <= INT32_MAX)
      query->topic_id_ = make_object<messageTopicForum>(static_cast<int32_t>(id));
    else if (TgFeatureParseNumber(TgFeatureParam(params, "threadId"), id) && id > 0)
      query->topic_id_ = make_object<messageTopicThread>(id);
    else { finish("Reply", "The topic or thread is missing; the message was not sent.", false); return; }
    if (message) query->reply_to_ = make_object<inputMessageReplyToMessage>(message, nullptr, 0, "");
    query->input_message_content_ = GetMessageText(text);
    SendQuery(std::move(query), [request, params](Object object)
    {
      if (TgFeatureError(request, object)) return;
      ClientActionResult result;
      result.title = "Reply sent";
      result.text = "Your message was sent to this topic / discussion thread.";
      auto context = params; context["text"] = ""; context["from"] = "";
      result.items.push_back(TgFeatureItem("Refresh conversation",
        TgFeatureParam(params, "topicId").empty() ? "tg.thread.history" : "tg.topic.history", context));
      request->onResult(std::move(result));
    });
    return;
  }

  if (operation == "tg.search")
  {
    ClientActionResult result;
    result.title = "Search Telegram";
    result.text = "Searches Telegram's available history. Secret Chats have a separate device-local search. Sender and date filters are applied to each page; use More results to continue.";
    auto context = params; context["offset"] = ""; context["from"] = ""; context["scope"] = ""; context["senderResolved"] = "";
    context["topicId"] = ""; context["threadId"] = "";
    result.items.push_back(TgFeatureItem("Search all Telegram chats", "tg.search.run", context,
      {{"query", "Search text (may be blank with a type filter)", ""},
       {"type", "Type: all/photo/video/audio/file/voice/video note/link/poll", "all"},
       {"sender", "Sender name, @username or Telegram numeric ID (optional)", ""},
       {"after", "From date YYYY-MM-DD (optional)", ""}, {"before", "Through date YYYY-MM-DD (optional)", ""}}));
    if (chat)
    {
      context = params;
      context["offset"] = ""; context["from"] = ""; context["senderResolved"] = "";
      context["scope"] = "chat";
      result.items.push_back(TgFeatureItem("Search this chat or topic", "tg.search.run", context,
        {{"query", "Search text", ""}, {"type", "Type: all/photo/video/audio/file/voice/video note/link/poll/pinned", "all"},
         {"sender", "Sender name, @username or numeric ID (optional)", ""},
         {"after", "From date YYYY-MM-DD (optional)", ""}, {"before", "Through date YYYY-MM-DD (optional)", ""}}));
    }
    context = params;
    context["scope"] = "secret";
    context["offset"] = ""; context["from"] = ""; context["senderResolved"] = "";
    context["topicId"] = ""; context["threadId"] = "";
    result.items.push_back(TgFeatureItem("Search Secret Chats on this device", "tg.search.run", context,
      {{"query", "Search text", ""}, {"type", "Type: all/photo/video/audio/file/voice/video note/link/poll", "all"},
       {"sender", "Sender name or ID (optional)", ""},
       {"after", "From date YYYY-MM-DD (optional)", ""}, {"before", "Through date YYYY-MM-DD (optional)", ""}}));
    request->onResult(std::move(result));
    return;
  }

  if (operation == "tg.search.run")
  {
    int32_t after = 0, before = 0;
    if (!TgFeatureParseDate(TgFeatureParam(params, "after"), after) ||
        !TgFeatureParseDate(TgFeatureParam(params, "before"), before, true) || (after && before && after > before))
    { finish("Search", "Use valid dates in YYYY-MM-DD order.", false); return; }
    const auto type = TgFeatureParam(params, "type");
    const std::set<std::string> types = {"", "all", "photo", "video", "audio", "file", "voice", "video note", "link", "poll", "pinned"};
    if (!types.count(type)) { finish("Search", "Choose a listed message type.", false); return; }
    int64_t sender = 0;
    const auto senderText = TgFeatureParam(params, "sender");
    if (!senderText.empty() && TgFeatureParam(params, "senderResolved").empty())
    {
      if (!TgFeatureParseNumber(senderText, sender))
      {
        if (senderText.front() == '@')
        {
          SendQuery(make_object<searchPublicChat>(senderText.substr(1)), [this, request](Object object)
          {
            if (TgFeatureError(request, object)) return;
            auto contact = td::move_tl_object_as<td::td_api::chat>(object);
            if (contact->type_->get_id() != chatTypePrivate::ID)
            {
              ClientActionResult result; result.success = false; result.title = "Search";
              result.text = "The sender username must identify a person or bot."; request->onResult(std::move(result)); return;
            }
            auto followup = std::make_shared<ClientActionRequest>(*request);
            followup->params["senderResolved"] = std::to_string(static_cast<chatTypePrivate&>(*contact->type_).user_id_);
            PerformClientAction(followup);
          });
          return;
        }
        std::vector<int64_t> matches;
        for (const auto& contact : m_ContactInfos)
          if (contact.first > 0 && StrUtil::ToLower(contact.second.name).find(StrUtil::ToLower(senderText)) != std::string::npos)
            matches.push_back(contact.first);
        if (matches.size() != 1)
        {
          ClientActionResult result;
          result.title = "Choose sender";
          for (auto id : matches)
          {
            auto context = params; context["senderResolved"] = std::to_string(id);
            result.items.push_back(TgFeatureItem(GetContactName(id), "tg.search.run", context));
          }
          if (matches.empty()) result.text = "No cached name matches. Enter a Telegram @username or numeric user ID.";
          request->onResult(std::move(result));
          return;
        }
        sender = matches.front();
      }
    }
    else if (!TgFeatureParam(params, "senderResolved").empty())
      TgFeatureParseNumber(TgFeatureParam(params, "senderResolved"), sender);

    const bool inChat = TgFeatureParam(params, "scope") == "chat";
    const auto knownChat = m_ChatTypes.find(chat);
    const bool secret = TgFeatureParam(params, "scope") == "secret" ||
      (inChat && knownChat != m_ChatTypes.end() && knownChat->second == ChatSecret);
    const bool secretText = secret && !TgFeatureParam(params, "query").empty();
    if (!inChat && secret && !secretText)
    { finish("Search", "Enter search text to search across Secret Chats.", false); return; }
    if (!inChat && !secret && type == "pinned")
    { finish("Search", "Pinned-message search is available under Search this chat.", false); return; }
    const bool chatResults = inChat && !secretText;
    auto handler = [showMessages, request, params, chatResults, sender, after, before](Object object)
    {
      if (TgFeatureError(request, object)) return;
      std::vector<object_ptr<td::td_api::message>> values;
      auto next = params;
      bool more = false;
      if (chatResults)
      {
        auto found = td::move_tl_object_as<foundChatMessages>(object);
        next["from"] = StrUtil::NumToHex(found->next_from_message_id_);
        more = found->next_from_message_id_ != 0 && TgFeatureParam(next, "from") != TgFeatureParam(params, "from");
        values = std::move(found->messages_);
      }
      else
      {
        auto found = td::move_tl_object_as<foundMessages>(object);
        next["offset"] = found->next_offset_;
        more = !found->next_offset_.empty() && found->next_offset_ != TgFeatureParam(params, "offset");
        values = std::move(found->messages_);
      }
      std::vector<object_ptr<td::td_api::message>> filtered;
      for (auto& value : values)
      {
        if (!value || (after && value->date_ < after) || (before && value->date_ > before)) continue;
        if (sender && (!value->sender_id_ || value->sender_id_->get_id() != messageSenderUser::ID ||
            static_cast<messageSenderUser&>(*value->sender_id_).user_id_ != sender)) continue;
        filtered.push_back(std::move(value));
      }
      showMessages(std::move(filtered), "Telegram search results", params, more ? "tg.search.run" : "", next);
    };
    if (secretText)
    {
      auto query = make_object<searchSecretMessages>();
      query->chat_id_ = inChat ? chat : 0;
      query->query_ = TgFeatureParam(params, "query"); query->offset_ = TgFeatureParam(params, "offset");
      query->limit_ = 50; query->filter_ = TgFeatureFilter(type);
      SendQuery(std::move(query), handler);
    }
    else if (inChat)
    {
      auto query = make_object<searchChatMessages>();
      query->chat_id_ = chat; query->query_ = TgFeatureParam(params, "query"); query->limit_ = 50;
      query->from_message_id_ = StrUtil::NumFromHex<int64_t>(TgFeatureParam(params, "from"));
      if (sender && !secret) query->sender_id_ = make_object<messageSenderUser>(sender);
      query->filter_ = TgFeatureFilter(type);
      int64_t topic = 0;
      if (TgFeatureParseNumber(TgFeatureParam(params, "topicId"), topic))
        query->topic_id_ = make_object<messageTopicForum>(static_cast<int32_t>(topic));
      else if (TgFeatureParseNumber(TgFeatureParam(params, "threadId"), topic))
        query->topic_id_ = make_object<messageTopicThread>(topic);
      SendQuery(std::move(query), handler);
    }
    else
    {
      auto query = make_object<searchMessages>();
      query->query_ = TgFeatureParam(params, "query"); query->offset_ = TgFeatureParam(params, "offset");
      query->limit_ = 50; query->filter_ = TgFeatureFilter(type); query->min_date_ = after; query->max_date_ = before;
      SendQuery(std::move(query), handler);
    }
    return;
  }

  if (operation == "tg.draft.get" || operation == "tg.draft.set")
  {
    if (!chat) { finish("Cloud draft", "Select a chat first.", false); return; }
    int64_t topic = 0;
    const bool isTopic = TgFeatureParseNumber(TgFeatureParam(params, "topicId"), topic) && topic > 0;
    int64_t thread = 0;
    const bool isThread = TgFeatureParseNumber(TgFeatureParam(params, "threadId"), thread) && thread > 0;
    const auto chatType = m_ChatTypes.find(chat);
    const bool secret = chatType != m_ChatTypes.end() && chatType->second == ChatSecret;
    if (operation == "tg.draft.set")
    {
      auto query = make_object<setChatDraftMessage>();
      query->chat_id_ = chat;
      if (isTopic) query->topic_id_ = make_object<messageTopicForum>(static_cast<int32_t>(topic));
      else if (isThread) query->topic_id_ = make_object<messageTopicThread>(thread);
      const auto text = TgFeatureParam(params, "text");
      if (!text.empty())
      {
        query->draft_message_ = make_object<draftMessage>();
        auto content = make_object<draftMessageContentText>();
        content->text_ = GetFormattedText(text);
        query->draft_message_->content_ = std::move(content);
      }
      SendQuery(std::move(query), [request, finish, secret](Object object)
        { if (!TgFeatureError(request, object)) finish(secret ? "Local Secret Chat draft" : "Cloud draft",
            secret ? "Draft saved on this nchat device." : "Draft saved to Telegram."); });
      return;
    }
    auto receive = [request, params, secret](const draftMessage* draft)
    {
      std::string text;
      if (draft && draft->content_ && draft->content_->get_id() == draftMessageContentText::ID)
      {
        auto& content = static_cast<const draftMessageContentText&>(*draft->content_);
        if (content.text_) text = content.text_->text_;
      }
      ClientActionResult result;
      result.title = secret ? "Secret Chat draft on this device" : "Telegram cloud draft";
      result.text = text.empty() ? "No text draft saved for this chat / topic." : text;
      result.items.push_back(TgFeatureItem("Edit draft", "tg.draft.set", params, {{"text", "Draft text (blank clears)", text}}));
      if (TgFeatureParam(params, "topicId").empty() && TgFeatureParam(params, "threadId").empty())
      {
        if (!text.empty())
        {
          auto context = params; context["text"] = text;
          result.items.push_back(TgFeatureItem("Load draft into textbox", "local.compose", context));
        }
        result.items.push_back(TgFeatureItem("Save current textbox as Telegram draft", "tg.draft.set", params,
          {{"text", "Draft text (blank clears)", TgFeatureParam(params, "draft")}}, true));
      }
      request->onResult(std::move(result));
    };
    if (isTopic)
      SendQuery(make_object<getForumTopic>(chat, static_cast<int32_t>(topic)), [request, receive](Object object)
        { if (!TgFeatureError(request, object)) receive(static_cast<forumTopic&>(*object).draft_message_.get()); });
    else if (isThread)
      SendQuery(make_object<getMessageThread>(chat, thread), [request, receive](Object object)
        { if (!TgFeatureError(request, object)) receive(static_cast<messageThreadInfo&>(*object).draft_message_.get()); });
    else
      SendQuery(make_object<getChat>(chat), [request, receive](Object object)
        { if (!TgFeatureError(request, object)) receive(static_cast<td::td_api::chat&>(*object).draft_message_.get()); });
    return;
  }


  finish("Telegram tools", "This action is not available in the current build.", false);
}
