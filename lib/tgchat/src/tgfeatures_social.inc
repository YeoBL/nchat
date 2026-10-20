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





  fail("This Telegram action is not supported.");
  return true;
}
