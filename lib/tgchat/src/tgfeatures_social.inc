// Telegram story and device-local Secret Chat actions for the account-tools UI.
namespace
{
std::string TgSocialParam(const std::map<std::string, std::string>& params, const std::string& key)
{
  auto it = params.find(key);
  return (it == params.end()) ? std::string() : it->second;
}

ClientActionItem TgSocialItem(const std::string& title, const std::string& operation,
                              const std::map<std::string, std::string>& params = {},
                              const std::vector<ClientActionField>& fields = {}, bool confirm = false)
{
  ClientActionItem item;
  item.title = title;
  item.operation = operation;
  // The account-tools UI merges item parameters with its current context.
  // Cross-screen actions must explicitly clear unrelated routing/form values.
  for (const std::string key : {"chatId", "msgId", "topicId", "threadId", "topicName", "sourceChatId",
       "sourceMsgId", "from", "offset", "offsetDate", "offsetMessage", "offsetTopic", "storyId",
       "contact", "username", "text", "seconds", "path", "caption", "privacy", "duration"})
    item.params[key] = "";
  for (const auto& param : params) item.params[param.first] = param.second;
  item.fields = fields;
  item.confirm = confirm;
  return item;
}

std::string TgSocialDate(int32_t date)
{
  return TimeUtil::GetTimeString(int64_t(date) * 1000, false);
}
}

bool TgChat::Impl::HandleSocialAction(std::shared_ptr<ClientActionRequest> request)
{
  const std::string op = request->operation;
  if (op.compare(0, 8, "tg.story") && op.compare(0, 10, "tg.stories") && op.compare(0, 9, "tg.secret"))
  {
    return false;
  }

  auto done = [request](ClientActionResult result)
  {
    if (request->onResult) request->onResult(std::move(result));
  };
  auto fail = [done](const std::string& text)
  {
    ClientActionResult result;
    result.success = false;
    result.title = "Telegram";
    result.text = text;
    done(std::move(result));
  };
  auto error = [fail](const Object& object)
  {
    if (!object || object->get_id() == td::td_api::error::ID)
    {
      fail(object ? static_cast<const td::td_api::error&>(*object).message_ : "Telegram returned no result.");
      return true;
    }
    return false;
  };
  const auto params = request->params;
  const int64_t chatId = StrUtil::NumFromHex<int64_t>(TgSocialParam(params, "chatId"));

  if (op == "tg.stories.list")
  {
    // TDLib sends updateChatActiveStories as each page arrives. The map also
    // retains updates pushed while this screen is closed.
    auto stage = std::make_shared<int>(0);
    auto pages = std::make_shared<int>(0);
    auto warning = std::make_shared<std::string>();
    auto load = std::make_shared<std::function<void()>>();
    *load = [this, stage, pages, warning, load, done]()
    {
      if (*stage >= 2 || *pages >= 20)
      {
        ClientActionResult result;
        result.title = "Telegram stories";
        for (const auto& entry : m_ActiveStories)
        {
          if (entry.second.empty()) continue;
          auto item = TgSocialItem(GetContactName(entry.first), "tg.stories.chat",
                                  {{"chatId", StrUtil::NumToHex(entry.first)}});
          item.detail = std::to_string(entry.second.size()) + " stories";
          result.items.push_back(std::move(item));
        }
        if (result.items.empty()) result.text = "No active stories are available.";
        if (!warning->empty()) result.text += "\nSome stories could not refresh: " + *warning;
        result.items.push_back(TgSocialItem("Refresh / load more stories", "tg.stories.list"));
        result.items.push_back(TgSocialItem("Post a story", "tg.story.post"));
        const auto finish = done;
        *load = {};
        finish(std::move(result));
        return;
      }
      auto query = td::td_api::make_object<td::td_api::loadActiveStories>();
      query->story_list_ = (*stage == 0) ? td::td_api::object_ptr<td::td_api::StoryList>(
        td::td_api::make_object<td::td_api::storyListMain>()) :
        td::td_api::object_ptr<td::td_api::StoryList>(td::td_api::make_object<td::td_api::storyListArchive>());
      ++*pages;
      SendQuery(std::move(query), [stage, warning, load](Object object)
      {
        if (!object || object->get_id() == td::td_api::error::ID)
        {
          if (!object) *warning = "Telegram returned no result.";
          else if (static_cast<const td::td_api::error&>(*object).code_ != 404)
            *warning = static_cast<const td::td_api::error&>(*object).message_;
          ++*stage;
        }
        (*load)();
      });
    };
    (*load)();
    return true;
  }

  if (op == "tg.stories.chat")
  {
    if (!chatId) { fail("Choose a contact or channel first."); return true; }
    SendQuery(td::td_api::make_object<td::td_api::getChatActiveStories>(chatId),
      [this, chatId, done, error](Object object)
    {
      if (error(object)) return;
      const auto& stories = static_cast<const td::td_api::chatActiveStories&>(*object);
      ClientActionResult result;
      result.title = GetContactName(chatId) + " — stories";
      for (const auto& story : stories.stories_)
      {
        auto item = TgSocialItem("Story " + std::to_string(story->story_id_), "tg.story.open",
                                {{"chatId", StrUtil::NumToHex(chatId)}, {"storyId", std::to_string(story->story_id_)}});
        item.detail = TgSocialDate(story->date_) + (story->is_live_ ? " · Live" : "");
        result.items.push_back(std::move(item));
      }
      if (result.items.empty()) result.text = "This chat has no active stories.";
      done(std::move(result));
    });
    return true;
  }

  if (op == "tg.story.open")
  {
    int32_t storyId = 0;
    try { storyId = std::stoi(TgSocialParam(params, "storyId")); } catch (...) { }
    if (!chatId || storyId <= 0) { fail("Choose a story first."); return true; }
    SendQuery(td::td_api::make_object<td::td_api::getStory>(chatId, storyId, false),
      [this, chatId, done, error, fail](Object object)
    {
      if (error(object)) return;
      const auto& story = static_cast<const td::td_api::story&>(*object);
      ClientActionResult result;
      result.title = GetContactName(chatId) + " — " + TgSocialDate(story.date_);
      if (story.caption_) result.text = story.caption_->text_;
      int32_t fileId = 0;
      if (story.content_ && story.content_->get_id() == td::td_api::storyContentPhoto::ID)
      {
        const auto& photo = static_cast<const td::td_api::storyContentPhoto&>(*story.content_);
        int64_t largest = -1;
        if (photo.photo_) for (const auto& size : photo.photo_->sizes_)
        {
          if (!size) continue;
          const int64_t area = int64_t(size->width_) * size->height_;
          if (size->photo_ && area > largest) { largest = area; fileId = size->photo_->id_; }
        }
      }
      else if (story.content_ && story.content_->get_id() == td::td_api::storyContentVideo::ID)
      {
        const auto& video = static_cast<const td::td_api::storyContentVideo&>(*story.content_);
        if (video.video_ && video.video_->video_) fileId = video.video_->video_->id_;
      }
      if (!fileId)
      {
        result.text += "\nThis story's media type cannot be opened in a file viewer.";
        done(std::move(result));
        return;
      }
      auto download = td::td_api::make_object<td::td_api::downloadFile>();
      download->file_id_ = fileId;
      download->priority_ = 32;
      download->synchronous_ = true;
      SendQuery(std::move(download), [done, error, fail, result](Object fileObject) mutable
      {
        if (error(fileObject)) return;
        const auto& file = static_cast<const td::td_api::file&>(*fileObject);
        if (!file.local_ || !file.local_->is_downloading_completed_ || file.local_->path_.empty())
        {
          fail("The story media has not finished downloading. Try opening it again.");
          return;
        }
        result.filePath = file.local_->path_;
        done(std::move(result));
      });
    });
    return true;
  }

  if (op == "tg.story.post")
  {
    const std::string path = FileUtil::ExpandPath(TgSocialParam(params, "path"));
    if (path.empty())
    {
      ClientActionResult result;
      result.title = "Post a Telegram story";
      result.text = "Post for 24 hours; the default audience is your contacts. Telegram requires a photo at 1080×1920 (up to 10 MB), or a streamable 720×1280 H.265 MP4 video with one keyframe per second.";
      result.items.push_back(TgSocialItem("Choose story details", "tg.story.post", {},
        {{"path", "Photo / MP4 file path", ""}, {"caption", "Caption", ""},
         {"privacy", "Audience: contacts / close_friends / everyone", "contacts"},
         {"duration", "Video duration in seconds (photos: leave 0)", "0"}}, true));
      done(std::move(result));
      return true;
    }
    struct stat fileStat;
    if (stat(path.c_str(), &fileStat) || !S_ISREG(fileStat.st_mode))
    {
      fail("Choose an existing local photo or MP4 file.");
      return true;
    }
    const std::string mime = FileUtil::GetMimeType(path);
    const bool isVideo = (mime.compare(0, 6, "video/") == 0);
    if (!isVideo && mime.compare(0, 6, "image/"))
    {
      fail("Stories require a photo or MP4 video file.");
      return true;
    }
    if (!isVideo && fileStat.st_size > 10 * 1024 * 1024)
    {
      fail("Story photos must be at most 10 MB and 1080×1920 pixels.");
      return true;
    }
    auto post = td::td_api::make_object<td::td_api::postStory>();
    post->chat_id_ = m_SelfUserId;
    post->active_period_ = 86400;
    post->caption_ = GetFormattedText(TgSocialParam(params, "caption"));
    const std::string privacy = TgSocialParam(params, "privacy");
    if (privacy.empty() || privacy == "contacts")
      post->privacy_settings_ = td::td_api::make_object<td::td_api::storyPrivacySettingsContacts>();
    else if (privacy == "close_friends")
      post->privacy_settings_ = td::td_api::make_object<td::td_api::storyPrivacySettingsCloseFriends>();
    else if (privacy == "everyone")
      post->privacy_settings_ = td::td_api::make_object<td::td_api::storyPrivacySettingsEveryone>();
    else { fail("Audience must be contacts, close_friends, or everyone."); return true; }
    if (isVideo)
    {
      double duration = 0;
      try
      {
        const auto value = TgSocialParam(params, "duration");
        size_t consumed = 0;
        duration = std::stod(value, &consumed);
        if (consumed != value.size()) duration = 0;
      }
      catch (...) { }
      if (!(duration > 0 && duration <= 60))
      {
        fail("Enter a video duration greater than 0 and no longer than 60 seconds. Telegram requires a 720×1280 H.265 MP4 with one keyframe per second.");
        return true;
      }
      auto content = td::td_api::make_object<td::td_api::inputStoryContentVideo>();
      content->video_ = td::td_api::make_object<td::td_api::inputFileLocal>(path);
      content->duration_ = duration;
      post->content_ = std::move(content);
    }
    else
    {
      auto content = td::td_api::make_object<td::td_api::inputStoryContentPhoto>();
      content->photo_ = td::td_api::make_object<td::td_api::inputFileLocal>(path);
      post->content_ = std::move(content);
    }
    SendQuery(std::move(post), [done, error](Object object)
    {
      if (error(object)) return;
      ClientActionResult result;
      result.title = "Story submitted";
      result.text = "Telegram is uploading and processing your story.";
      result.items.push_back(TgSocialItem("View my stories", "tg.stories.chat",
        {{"chatId", StrUtil::NumToHex(static_cast<const td::td_api::story&>(*object).poster_chat_id_)}}));
      done(std::move(result));
    });
    return true;
  }

  if (op == "tg.secret.list")
  {
    // The chat/type maps belong to the TDLib response thread. Render after a
    // local query callback rather than racing updateNewChat on the UI thread.
    SendQuery(td::td_api::make_object<td::td_api::getChats>(nullptr, 1), [this, done, error](Object object)
    {
      if (error(object)) return;
      ClientActionResult result;
      result.title = "Secret Chats on this nchat device";
      result.text = "Secret Chats are device-specific. Chats from your phone cannot be imported here. nchat stores message history in its own unencrypted local cache.";
      for (const auto& entry : m_ChatTypes)
      {
        if (entry.second != ChatSecret) continue;
        result.items.push_back(TgSocialItem(GetContactName(entry.first), "tg.secret.open",
                                            {{"chatId", StrUtil::NumToHex(entry.first)}}));
      }
      result.items.push_back(TgSocialItem("Start a new Secret Chat", "tg.secret.new", {},
        {{"contact", "Telegram @username or user ID (decimal)", ""}}, true));
      done(std::move(result));
    });
    return true;
  }

  if (op == "tg.secret.new")
  {
    std::string contact = TgSocialParam(params, "contact");
    if (!params.count("contact")) contact = TgSocialParam(params, "username");
    StrUtil::Trim(contact);
    if (contact.empty() && !chatId)
    {
      ClientActionResult result;
      result.title = "Start a Secret Chat";
      result.items.push_back(TgSocialItem("Choose contact", "tg.secret.new", {},
        {{"contact", "Telegram @username or user ID (decimal)", ""}}, true));
      done(std::move(result));
      return true;
    }
    auto create = [this, done, error, fail](int64_t userId)
    {
      if (userId <= 0 || userId == m_SelfUserId)
      {
        fail("Choose another Telegram user to start a Secret Chat.");
        return;
      }
      SendQuery(td::td_api::make_object<td::td_api::createNewSecretChat>(userId),
        [this, done, error](Object object)
      {
        if (error(object)) return;
        const auto& chat = static_cast<const td::td_api::chat&>(*object);
        const std::string id = StrUtil::NumToHex(chat.id_);
        ClientActionResult result;
        result.title = "Secret Chat created";
        result.text = "Waiting for the other person to accept the encrypted chat.";
        result.items.push_back(TgSocialItem("Open Secret Chat", "tg.secret.open", {{"chatId", id}}));
        result.items.push_back(TgSocialItem("Open in chat view", "navigate", {{"chatId", id}}));
        CreateChat(std::move(object));
        done(std::move(result));
      });
    };
    if (contact.empty())
    {
      SendQuery(td::td_api::make_object<td::td_api::getChat>(chatId),
        [create, error, fail](Object object)
      {
        if (error(object)) return;
        const auto& chat = static_cast<const td::td_api::chat&>(*object);
        if (!chat.type_ || chat.type_->get_id() != td::td_api::chatTypePrivate::ID)
        {
          fail("Start a Secret Chat from a private contact, or enter their @username.");
          return;
        }
        create(static_cast<const td::td_api::chatTypePrivate&>(*chat.type_).user_id_);
      });
    }
    else
    {
      int64_t userId = 0;
      try
      {
        size_t consumed = 0;
        userId = std::stoll(contact, &consumed);
        if (consumed != contact.size()) userId = 0;
      }
      catch (...) { }
      if (userId > 0) create(userId);
      else
      {
        const std::string username = (contact[0] == '@') ? contact.substr(1) : contact;
        SendQuery(td::td_api::make_object<td::td_api::searchPublicChat>(username),
          [create, error, fail](Object object)
        {
          if (error(object)) return;
          const auto& chat = static_cast<const td::td_api::chat&>(*object);
          if (!chat.type_ || chat.type_->get_id() != td::td_api::chatTypePrivate::ID)
          {
            fail("Secret Chats require a person's account, not a group or channel.");
            return;
          }
          create(static_cast<const td::td_api::chatTypePrivate&>(*chat.type_).user_id_);
        });
      }
    }
    return true;
  }

  if (op == "tg.secret.open" || op == "tg.secret.send" || op == "tg.secret.timer" || op == "tg.secret.close")
  {
    if (!chatId) { fail("Choose a Secret Chat first."); return true; }
    SendQuery(td::td_api::make_object<td::td_api::getChat>(chatId),
      [this, chatId, op, params, done, error, fail](Object object)
    {
      if (error(object)) return;
      const auto& chat = static_cast<const td::td_api::chat&>(*object);
      if (!chat.type_ || chat.type_->get_id() != td::td_api::chatTypeSecret::ID)
      {
        fail("This action requires a Secret Chat created on this nchat device.");
        return;
      }
      const int32_t secretId = static_cast<const td::td_api::chatTypeSecret&>(*chat.type_).secret_chat_id_;
      const std::string title = chat.title_;
      if (op == "tg.secret.send")
      {
        const std::string text = TgSocialParam(params, "text");
        if (text.empty()) { fail("Enter a message to send."); return; }
        auto send = td::td_api::make_object<td::td_api::sendMessage>();
        send->chat_id_ = chatId;
        send->input_message_content_ = GetMessageText(text);
        SendQuery(std::move(send), [chatId, done, error](Object sent)
        {
          if (error(sent)) return;
          ClientActionResult result;
          result.title = "Secret message sent";
          result.items.push_back(TgSocialItem("Return to Secret Chat", "tg.secret.open",
                                               {{"chatId", StrUtil::NumToHex(chatId)}}));
          done(std::move(result));
        });
        return;
      }
      if (op == "tg.secret.close")
      {
        SendQuery(td::td_api::make_object<td::td_api::closeSecretChat>(secretId), [done, error](Object closed)
        {
          if (error(closed)) return;
          ClientActionResult result;
          result.title = "Secret Chat closed";
          result.text = "This encrypted chat can no longer receive new messages.";
          done(std::move(result));
        });
        return;
      }
      if (op == "tg.secret.timer")
      {
        int seconds = -1;
        try
        {
          const auto value = TgSocialParam(params, "seconds");
          size_t consumed = 0;
          seconds = std::stoi(value, &consumed);
          if (consumed != value.size()) seconds = -1;
        }
        catch (...) { }
        if (seconds < 0 || seconds > 604800)
        {
          fail("Enter a self-destruct timer from 0 to 604800 seconds; 0 disables it.");
          return;
        }
        SendQuery(td::td_api::make_object<td::td_api::setChatMessageAutoDeleteTime>(chatId, seconds),
          [chatId, done, error](Object updated)
        {
          if (error(updated)) return;
          ClientActionResult result;
          result.title = "Secret Chat timer updated";
          result.items.push_back(TgSocialItem("Return to Secret Chat", "tg.secret.open",
                                               {{"chatId", StrUtil::NumToHex(chatId)}}));
          done(std::move(result));
        });
        return;
      }
      const int timer = chat.message_auto_delete_time_;
      SendQuery(td::td_api::make_object<td::td_api::getSecretChat>(secretId),
        [this, chatId, title, params, timer, done, error](Object secretObject)
      {
        if (error(secretObject)) return;
        const auto& secret = static_cast<const td::td_api::secretChat&>(*secretObject);
        const bool ready = secret.state_ && secret.state_->get_id() == td::td_api::secretChatStateReady::ID;
        ClientActionResult initial;
        initial.title = title + " — Secret Chat";
        initial.text = ready ? "Encrypted chat · Ready" :
          (secret.state_ && secret.state_->get_id() == td::td_api::secretChatStatePending::ID ?
           "Waiting for acceptance" : "Closed");
        initial.text += "\nSelf-destruct timer: " + (timer ? std::to_string(timer) + " seconds" : "Off");
        initial.text += "\nnchat also stores this history in its own unencrypted local cache.";
        if (AppConfig::GetNum("message_delete") == MessageDeletePrefix)
          initial.text += "\nYour nchat deletion setting retains deleted messages locally. Telegram's self-destruct timer does not erase those retained copies.";
        if (!secret.key_hash_.empty()) initial.text += "\nKey fingerprint: " + StrUtil::StrToHex(secret.key_hash_);
        initial.items.push_back(TgSocialItem("Open in chat view", "navigate", {{"chatId", StrUtil::NumToHex(chatId)}}));
        if (ready)
        {
          initial.items.push_back(TgSocialItem("Write a message", "tg.secret.send",
            {{"chatId", StrUtil::NumToHex(chatId)}}, {{"text", "Message", ""}}));
          initial.items.push_back(TgSocialItem("Set self-destruct timer", "tg.secret.timer",
            {{"chatId", StrUtil::NumToHex(chatId)}}, {{"seconds", "Seconds (0 disables)", std::to_string(timer)}}, true));
          initial.items.push_back(TgSocialItem("Close this Secret Chat", "tg.secret.close",
            {{"chatId", StrUtil::NumToHex(chatId)}}, {}, true));
        }
        int64_t from = StrUtil::NumFromHex<int64_t>(TgSocialParam(params, "from"));
        // TDLib message IDs encode their type; retain a valid boundary ID and
        // filter the inclusive boundary instead of subtracting from the ID.
        SendQuery(td::td_api::make_object<td::td_api::getChatHistory>(chatId, from, 0, from ? 31 : 30, false),
          [this, chatId, from, done, error, initial](Object history) mutable
        {
          if (error(history)) return;
          auto& messages = static_cast<td::td_api::messages&>(*history);
          int64_t oldest = 0;
          for (auto& message : messages.messages_)
          {
            if (!message || message->id_ == from) continue;
            oldest = message->id_;
            ChatMessage converted;
            TdMessageConvert(*message, converted);
            const auto heading = TgSocialDate(message->date_) + " · " +
              (message->is_outgoing_ ? "You" : GetContactName(StrUtil::NumFromHex<int64_t>(converted.senderId)));
            initial.text += "\n\n" + heading + "\n" + converted.text;
            auto item = TgSocialItem(heading,
              "tg.secret.message", {{"chatId", StrUtil::NumToHex(chatId)}, {"msgId", StrUtil::NumToHex(message->id_)}});
            item.detail = converted.text;
            initial.items.push_back(std::move(item));
          }
          if (oldest && oldest != from)
            initial.items.push_back(TgSocialItem("Older messages", "tg.secret.open",
              {{"chatId", StrUtil::NumToHex(chatId)}, {"from", StrUtil::NumToHex(oldest)}}));
          done(std::move(initial));
        });
      });
    });
    return true;
  }

  if (op == "tg.secret.message" || op == "tg.secret.file")
  {
    const int64_t msgId = StrUtil::NumFromHex<int64_t>(TgSocialParam(params, "msgId"));
    if (!chatId || !msgId) { fail("Choose a message first."); return true; }
    SendQuery(td::td_api::make_object<td::td_api::getMessage>(chatId, msgId),
      [this, chatId, msgId, op, done, error, fail](Object object)
    {
      if (error(object)) return;
      auto& message = static_cast<td::td_api::message&>(*object);
      const bool disappearing = (message.self_destruct_type_ != nullptr || message.self_destruct_in_ > 0);
      ChatMessage converted;
      TdMessageConvert(message, converted);
      ClientActionResult result;
      result.title = (message.is_outgoing_ ? "You" : GetContactName(StrUtil::NumFromHex<int64_t>(converted.senderId))) +
                     " · " + TgSocialDate(message.date_);
      result.text = converted.text;
      if (converted.fileInfo.empty()) { done(std::move(result)); return; }
      if (disappearing)
      {
        result.text += "\nTimed attachments cannot be opened in an external viewer because their expiry cannot be enforced there.";
        done(std::move(result));
        return;
      }
      if (op == "tg.secret.message")
      {
        result.items.push_back(TgSocialItem("Open attachment", "tg.secret.file",
          {{"chatId", StrUtil::NumToHex(chatId)}, {"msgId", StrUtil::NumToHex(msgId)}}));
        done(std::move(result));
        return;
      }
      const auto info = ProtocolUtil::FileInfoFromHex(converted.fileInfo);
      if (info.fileStatus == FileStatusDownloaded && !info.filePath.empty() && FileUtil::Exists(info.filePath))
      {
        result.filePath = info.filePath;
        done(std::move(result));
        return;
      }
      auto remote = td::td_api::make_object<td::td_api::getRemoteFile>();
      remote->remote_file_id_ = info.fileId;
      SendQuery(std::move(remote), [this, done, error, fail, result](Object remoteObject) mutable
      {
        if (error(remoteObject)) return;
        auto download = td::td_api::make_object<td::td_api::downloadFile>();
        download->file_id_ = static_cast<const td::td_api::file&>(*remoteObject).id_;
        download->priority_ = 32;
        download->synchronous_ = true;
        SendQuery(std::move(download), [done, error, fail, result](Object fileObject) mutable
        {
          if (error(fileObject)) return;
          const auto& file = static_cast<const td::td_api::file&>(*fileObject);
          if (!file.local_ || !file.local_->is_downloading_completed_ || file.local_->path_.empty())
          {
            fail("The attachment has not finished downloading. Try again shortly.");
            return;
          }
          result.filePath = file.local_->path_;
          done(std::move(result));
        });
      });
    });
    return true;
  }

  fail("This Telegram action is not supported.");
  return true;
}
