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









  finish("Telegram tools", "This action is not available in the current build.", false);
}
