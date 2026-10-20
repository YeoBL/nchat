// Telegram account tools share the terminal's labelled list/form interface.
#include <cassert>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace TgManage
{
static std::string Trim(const std::string& value)
{
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

static bool Integer(const std::string& value, int64_t& number)
{
  if (value.empty()) return false;
  size_t consumed = 0;
  try { number = std::stoll(value, &consumed); }
  catch (...) { return false; }
  return consumed == value.size();
}

static bool Decimal(const std::string& value, double& number)
{
  size_t consumed = 0;
  try { number = std::stod(value, &consumed); }
  catch (...) { return false; }
  return consumed == value.size() && std::isfinite(number);
}

static bool HexId(const std::string& value, int64_t& number)
{
  if (value.empty() || value.size() > 40 || value.size() % 2 ||
      value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  // nchat encodes the decimal ID's UTF-8 bytes as hex, not the integer itself.
  return Integer(StrUtil::StrFromHex(value), number) && number != 0;
}

static bool Yes(const std::string& value)
{
  return value == "1" || value == "yes" || value == "Yes" || value == "true";
}

static bool Boolean(const std::string& value)
{
  return Yes(value) || value.empty() || value == "0" || value == "no" || value == "No" || value == "false";
}

static std::vector<std::string> Options(const std::string& text)
{
  std::vector<std::string> options;
  size_t first = 0;
  for (size_t index = 0; index <= text.size(); ++index)
  {
    if (index == text.size() || text[index] == '|' || text[index] == '\n')
    {
      options.push_back(Trim(text.substr(first, index - first)));
      first = index + 1;
    }
  }
  return options;
}

static bool ScheduledTime(const std::string& value, int32_t& timestamp)
{
  std::tm local{};
  local.tm_isdst = -1;
  std::istringstream input(value);
  input >> std::get_time(&local, "%Y-%m-%d %H:%M");
  if (input.fail() || input.peek() != std::char_traits<char>::eof()) return false;
  const std::time_t parsed = std::mktime(&local);
  char roundTrip[32]{};
  std::strftime(roundTrip, sizeof(roundTrip), "%Y-%m-%d %H:%M", &local);
  if (value != roundTrip || parsed < std::time(nullptr) + 10 ||
      parsed > std::numeric_limits<int32_t>::max()) return false;
  timestamp = static_cast<int32_t>(parsed);
  return true;
}

static std::string Status(const td::td_api::ChatMemberStatus* status)
{
  if (!status) return "Unknown";
  switch (status->get_id())
  {
    case td::td_api::chatMemberStatusCreator::ID: return "Owner";
    case td::td_api::chatMemberStatusAdministrator::ID: return "Administrator";
    case td::td_api::chatMemberStatusRestricted::ID: return "Restricted";
    case td::td_api::chatMemberStatusLeft::ID: return "Left";
    case td::td_api::chatMemberStatusBanned::ID: return "Banned";
    default: return "Member";
  }
}

// This small check covers the boundary parsers without contacting Telegram.
static void CheckParsers()
{
#ifndef NDEBUG
  static const bool checked = []
  {
    int64_t integer = 0;
    double decimal = 0;
    int32_t timestamp = 0;
    assert(Integer("42", integer) && integer == 42 && !Integer("42x", integer));
    assert(HexId(StrUtil::NumToHex(int64_t(-1001234567890)), integer) && integer == -1001234567890);
    assert(Decimal("-0.25", decimal) && decimal == -0.25 && !Decimal("nan", decimal));
    assert(Options(" one | two\nthree ") == std::vector<std::string>({"one", "two", "three"}));
    assert(!ScheduledTime("2099-02-31 12:00", timestamp));
    assert(!ScheduledTime("2020-01-01 12:00 trailing", timestamp));
    return true;
  }();
  (void)checked;
#endif
}
}

bool TgChat::Impl::HandleManageAction(std::shared_ptr<ClientActionRequest> request)
{
  using namespace td::td_api;
  std::string operation = request->operation;
  if (operation == "send_recording")
  {
    const auto kind = request->params.find("mediaKind");
    if (kind != request->params.end() && kind->second == "voice") operation = "tg.send.voice";
    else if (kind != request->params.end() && kind->second == "video") operation = "tg.send.videoNote";
  }
  if (operation != "tg.manage" && operation.rfind("tg.chat.", 0) != 0 &&
      operation.rfind("tg.send.", 0) != 0 && operation != "tg.poll.create" &&
      operation != "tg.contact.send" && operation != "tg.location.send" &&
      operation != "tg.translate" && operation != "tg.buttons" &&
      operation != "tg.button.callback" && operation != "tg.button.reply" && operation != "tg.forward" && operation != "tg.forward.pick" &&
      operation != "forward_messages" && operation != "send_recording")
    return false;

  TgManage::CheckParsers();
  const auto params = request->params;
  const auto value = [params](const std::string& key, const std::string& fallback = std::string())
  {
    const auto found = params.find(key);
    return found == params.end() ? fallback : found->second;
  };
  const auto finish = [request](ClientActionResult result)
  {
    if (request->onResult) request->onResult(std::move(result));
  };
  const auto fail = [finish](const std::string& text)
  {
    ClientActionResult result;
    result.success = false;
    result.title = "Telegram";
    result.text = text;
    finish(std::move(result));
  };
  const auto checked = [fail](const Object& object)
  {
    if (!object) { fail("Telegram returned no result."); return false; }
    if (object->get_id() == error::ID)
    {
      fail(static_cast<const error&>(*object).message_);
      return false;
    }
    return true;
  };
  const auto complete = [finish, checked](const std::string& title, const std::string& text)
  {
    return [finish, checked, title, text](Object object)
    {
      if (!checked(object)) return;
      ClientActionResult result;
      result.title = title;
      result.text = text;
      finish(std::move(result));
    };
  };
  const auto item = [params](const std::string& title, const std::string& op,
                            const std::string& detail = std::string(), bool confirm = false)
  {
    ClientActionItem result;
    result.title = title;
    result.detail = detail;
    result.operation = op;
    result.params = params;
    result.confirm = confirm;
    return result;
  };

  int64_t chatId = 0;
  if (!TgManage::HexId(value("chatId"), chatId))
  {
    fail("Select a Telegram chat first.");
    return true;
  }
  int64_t msgId = 0;
  TgManage::HexId(value("msgId"), msgId);
  if (operation == "send_recording")
  { fail("Choose a voice or video recording."); return true; }

  if (operation == "tg.manage")
  {
    ClientActionResult result;
    result.title = "Telegram chat tools";
    result.items.push_back(item("Chat and group information", "tg.chat.info"));
    auto mute = item("Mute or unmute", "tg.chat.mute", "0 unmutes; 3600 mutes for an hour", true);
    mute.fields = {{"seconds", "Mute duration in seconds (0 = unmute)", "3600"}};
    result.items.push_back(std::move(mute));
    result.items.push_back(item("Mark unread", "tg.chat.unread", "Updates Telegram on your other devices", true));
    auto silent = item("Send silently", "tg.send.silent", "Sends without a notification sound", true);
    silent.fields = {{"sendText", "Message", value("draft")}};
    result.items.push_back(std::move(silent));
    auto schedule = item("Schedule a message", "tg.send.scheduled", "Telegram sends it at your chosen local time", true);
    schedule.fields = {{"sendText", "Message", value("draft")}, {"time", "Local date/time: YYYY-MM-DD HH:MM", ""}};
    result.items.push_back(std::move(schedule));
    auto poll = item("Create a poll or quiz", "tg.poll.create", "Separate answer options with |", true);
    poll.fields = {{"question", "Question", ""}, {"options", "Answers separated by |", ""},
                   {"multi", "Allow multiple answers (yes/no)", "no"},
                   {"anonymous", "Anonymous votes (yes/no)", "no"},
                   {"quiz", "Quiz (yes/no)", "no"}, {"correct", "Quiz correct answer number (starting at 1)", "1"}};
    result.items.push_back(std::move(poll));
    auto contact = item("Send a contact", "tg.contact.send", "Shares the entered name and phone number", true);
    contact.fields = {{"first", "First name", ""}, {"last", "Last name", ""}, {"phone", "Phone number with country code", ""}};
    result.items.push_back(std::move(contact));
    auto location = item("Send a location", "tg.location.send", "A fixed location, not live tracking", true);
    location.fields = {{"latitude", "Latitude (-90 to 90)", ""}, {"longitude", "Longitude (-180 to 180)", ""}};
    result.items.push_back(std::move(location));
    if (msgId)
    {
      auto translation = item("Translate selected message", "tg.translate");
      translation.fields = {{"language", "Target language code", "en"}};
      result.items.push_back(std::move(translation));
      result.items.push_back(item("Selected message's buttons", "tg.buttons"));
      result.items.push_back(item("Forward selected message", "tg.forward.pick"));
    }
    finish(std::move(result));
    return true;
  }

  if (operation == "tg.chat.mute")
  {
    int64_t duration = 0;
    if (!TgManage::Integer(value("seconds", "3600"), duration) || duration < 0 || duration > INT32_MAX)
    {
      fail("Enter a mute duration between 0 and 2147483647 seconds.");
      return true;
    }
    SendQuery(make_object<getChat>(chatId), [this, chatId, duration, checked, complete](Object object)
    {
      if (!checked(object)) return;
      auto chat = move_object_as<td::td_api::chat>(object);
      auto settings = std::move(chat->notification_settings_);
      if (!settings) settings = make_object<chatNotificationSettings>();
      settings->use_default_mute_for_ = false;
      settings->mute_for_ = static_cast<int32_t>(duration);
      SendQuery(make_object<setChatNotificationSettings>(chatId, std::move(settings)),
                complete("Notifications", duration == 0 ? "Chat unmuted." : "Chat muted."));
    });
    return true;
  }
  if (operation == "tg.chat.unread")
  {
    SendQuery(make_object<toggleChatIsMarkedAsUnread>(chatId, true), complete("Unread", "Chat marked unread."));
    return true;
  }

  if (operation == "tg.send.silent" || operation == "tg.send.scheduled" || operation == "tg.button.reply" ||
      operation == "tg.send.voice" || operation == "tg.send.videoNote" || operation == "tg.poll.create" ||
      operation == "tg.contact.send" || operation == "tg.location.send")
  {
    auto send = make_object<sendMessage>();
    send->chat_id_ = chatId;
    send->options_ = make_object<messageSendOptions>();
    int64_t topic = 0;
    if (!value("topicId").empty())
    {
      if (!TgManage::Integer(value("topicId"), topic) || topic <= 0 || topic > INT32_MAX)
      { fail("Invalid topic. Reopen the topic before sending."); return true; }
      send->topic_id_ = make_object<messageTopicForum>(static_cast<int32_t>(topic));
    }
    else if (!value("threadId").empty())
    {
      if (!TgManage::Integer(value("threadId"), topic) || topic <= 0)
      { fail("Invalid comment thread. Reopen the thread before sending."); return true; }
      send->topic_id_ = make_object<messageTopicThread>(topic);
    }
    if (operation == "tg.send.silent" || operation == "tg.send.scheduled" || operation == "tg.button.reply")
    {
      const auto text = value(operation == "tg.button.reply" ? "buttonText" : "sendText");
      if (TgManage::Trim(text).empty()) { fail("Enter a message to send."); return true; }
      if (operation == "tg.button.reply")
      {
        auto content = make_object<inputMessageText>();
        content->text_ = make_object<formattedText>();
        content->text_->text_ = text;
        send->input_message_content_ = std::move(content);
      }
      else send->input_message_content_ = GetMessageText(text);
      if (operation == "tg.send.silent") send->options_->disable_notification_ = true;
      else if (operation == "tg.send.scheduled")
      {
        int32_t timestamp = 0;
        if (!TgManage::ScheduledTime(value("time"), timestamp))
        {
          fail("Enter a valid future local date/time as YYYY-MM-DD HH:MM.");
          return true;
        }
        send->options_->scheduling_state_ = make_object<messageSchedulingStateSendAtDate>(timestamp, 0);
      }
    }
    else if (operation == "tg.send.voice" || operation == "tg.send.videoNote")
    {
      const auto path = FileUtil::ExpandPath(value("path"));
      struct stat fileStat{};
      double durationSeconds = 0;
      if (path.empty() || stat(path.c_str(), &fileStat) != 0 || !S_ISREG(fileStat.st_mode) || fileStat.st_size == 0)
      { fail("Choose an existing, nonempty media file."); return true; }
      if (!TgManage::Decimal(value("duration", value("durationSec", "0")), durationSeconds) ||
          durationSeconds < 0 || durationSeconds > INT32_MAX - 1)
      { fail("Enter a valid recording duration in seconds."); return true; }
      const int32_t duration = static_cast<int32_t>(std::ceil(durationSeconds));
      if (operation == "tg.send.voice")
      {
        auto voice = make_object<inputMessageVoiceNote>();
        voice->voice_note_ = make_object<inputFileLocal>(path);
        voice->duration_ = duration;
        send->input_message_content_ = std::move(voice);
      }
      else
      {
        if (duration > 60) { fail("Telegram video notes must be at most 60 seconds long."); return true; }
        int64_t length = 0;
        if (!TgManage::Integer(value("length", "384"), length) || length <= 0 || length > 640)
        { fail("Video notes must be square and at most 640 pixels wide."); return true; }
        auto video = make_object<inputMessageVideoNote>();
        video->video_note_ = make_object<inputFileLocal>(path);
        video->duration_ = duration;
        video->length_ = static_cast<int32_t>(length);
        send->input_message_content_ = std::move(video);
      }
    }
    else if (operation == "tg.poll.create")
    {
      const auto options = TgManage::Options(value("options"));
      if (TgManage::Trim(value("question")).empty() || options.size() < 2 || options.size() > 12 ||
          std::any_of(options.begin(), options.end(), [](const std::string& option) { return option.empty(); }))
      {
        fail("Enter a question and 2–12 nonempty answers separated by |.");
        return true;
      }
      if (std::set<std::string>(options.begin(), options.end()).size() != options.size())
      {
        fail("Poll answers must be distinct.");
        return true;
      }
      if (!TgManage::Boolean(value("multi")) || !TgManage::Boolean(value("quiz")) || !TgManage::Boolean(value("anonymous")))
      {
        fail("Answer yes or no for multiple answers, anonymous votes and quiz.");
        return true;
      }
      auto poll = make_object<inputMessagePoll>();
      poll->question_ = GetFormattedText(value("question"));
      poll->is_anonymous_ = TgManage::Yes(value("anonymous"));
      poll->allows_multiple_answers_ = TgManage::Yes(value("multi"));
      poll->allows_revoting_ = true;
      for (const auto& option : options)
        poll->options_.push_back(make_object<inputPollOption>(GetFormattedText(option), nullptr));
      if (TgManage::Yes(value("quiz")))
      {
        int64_t correct = 0;
        if (!TgManage::Integer(value("correct", "1"), correct) || correct < 1 || correct > static_cast<int64_t>(options.size()))
        {
          fail("Choose the correct quiz answer by its number, starting at 1.");
          return true;
        }
        if (poll->allows_multiple_answers_) { fail("This quiz form accepts one correct answer; choose no for multiple answers."); return true; }
        auto quiz = make_object<inputPollTypeQuiz>();
        quiz->correct_option_ids_.push_back(static_cast<int32_t>(correct - 1));
        poll->type_ = std::move(quiz);
        poll->allows_revoting_ = false;
      }
      else poll->type_ = make_object<inputPollTypeRegular>(false);
      send->input_message_content_ = std::move(poll);
    }
    else if (operation == "tg.contact.send")
    {
      const std::string phone = TgManage::Trim(value("phone"));
      if (TgManage::Trim(value("first")).empty() || phone.empty() ||
          phone.find_first_not_of("+0123456789 ()-") != std::string::npos ||
          std::count_if(phone.begin(), phone.end(), [](char c) { return c >= '0' && c <= '9'; }) < 3)
      {
        fail("Enter a contact's first name and valid phone number.");
        return true;
      }
      auto contact = make_object<inputMessageContact>();
      contact->contact_ = make_object<td::td_api::contact>(phone, value("first"), value("last"), "", 0);
      send->input_message_content_ = std::move(contact);
    }
    else
    {
      double latitude = 0, longitude = 0;
      if (!TgManage::Decimal(value("latitude"), latitude) || !TgManage::Decimal(value("longitude"), longitude) ||
          latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180)
      {
        fail("Enter a latitude between -90 and 90 and longitude between -180 and 180.");
        return true;
      }
      send->input_message_content_ = make_object<inputMessageLocation>(make_object<location>(latitude, longitude, 0));
    }
    SendQuery(std::move(send), [this, request, checked, finish, operation](Object object)
    {
      if (!checked(object)) return;
      auto message = move_object_as<td::td_api::message>(object);
      if (message->scheduling_state_ == nullptr)
      {
        auto notify = std::make_shared<SendMessageNotify>(m_ProfileId);
        notify->success = true;
        notify->chatId = StrUtil::NumToHex(message->chat_id_);
        TdMessageConvert(*message, notify->chatMessage);
        CallMessageHandler(notify);
      }
      ClientActionResult result;
      result.title = "Telegram";
      result.text = operation == "tg.send.scheduled" ? "Message scheduled in Telegram." : "Submitted to Telegram.";
      finish(std::move(result));
    });
    return true;
  }

  if (operation == "tg.translate")
  {
    if (!msgId) { fail("Select a message to translate."); return true; }
    const auto language = value("language", "en");
    if (language.empty() || language.size() > 12 || language.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-") != std::string::npos)
    {
      fail("Enter a language code such as en, fr or zh.");
      return true;
    }
    SendQuery(make_object<translateMessageText>(chatId, msgId, language, ""), [checked, finish](Object object)
    {
      if (!checked(object)) return;
      ClientActionResult result;
      result.title = "Translation";
      result.text = static_cast<const formattedText&>(*object).text_;
      finish(std::move(result));
    });
    return true;
  }

  if (operation == "tg.buttons")
  {
    if (!msgId) { fail("Select a message with buttons."); return true; }
    SendQuery(make_object<getMessage>(chatId, msgId), [checked, finish, item](Object object)
    {
      if (!checked(object)) return;
      const auto& message = static_cast<const td::td_api::message&>(*object);
      ClientActionResult result;
      result.title = "Message buttons";
      if (message.reply_markup_ && message.reply_markup_->get_id() == replyMarkupShowKeyboard::ID)
      {
        const auto& keyboard = static_cast<const replyMarkupShowKeyboard&>(*message.reply_markup_);
        for (const auto& row : keyboard.rows_)
        {
          for (const auto& button : row)
          {
            if (!button || !button->type_) continue;
            if (button->type_->get_id() == keyboardButtonTypeText::ID)
            {
              auto action = item(button->text_, "tg.button.reply", "Send this response to the bot", true);
              action.params["buttonText"] = button->text_;
              result.items.push_back(std::move(action));
            }
            else if (button->type_->get_id() == keyboardButtonTypeRequestPhoneNumber::ID)
              result.text += button->text_ + ": use Telegram to choose whether to share your phone number.\n";
            else if (button->type_->get_id() == keyboardButtonTypeRequestLocation::ID)
              result.text += button->text_ + ": use Telegram to choose whether to share your current location.\n";
            else if (button->type_->get_id() == keyboardButtonTypeWebApp::ID)
              result.text += button->text_ + ": open this Mini App in Telegram.\n";
            else result.text += button->text_ + ": this selection menu requires Telegram.\n";
          }
        }
        if (result.items.empty() && result.text.empty()) result.text = "This bot keyboard is empty.";
        finish(std::move(result));
        return;
      }
      if (!message.reply_markup_ || message.reply_markup_->get_id() != replyMarkupInlineKeyboard::ID)
      {
        result.text = "This message has no buttons.";
        finish(std::move(result));
        return;
      }
      const auto& keyboard = static_cast<const replyMarkupInlineKeyboard&>(*message.reply_markup_);
      for (const auto& row : keyboard.rows_)
      {
        for (const auto& button : row)
        {
          if (!button || !button->type_) continue;
          if (button->type_->get_id() == inlineKeyboardButtonTypeUrl::ID)
          {
            auto action = item(button->text_, "openUrl", "Open link");
            action.params["url"] = static_cast<const inlineKeyboardButtonTypeUrl&>(*button->type_).url_;
            result.items.push_back(std::move(action));
          }
          else if (button->type_->get_id() == inlineKeyboardButtonTypeCallback::ID)
          {
            auto action = item(button->text_, "tg.button.callback", "Send this button to the bot", true);
            action.params["data"] = StrUtil::StrToHex(static_cast<const inlineKeyboardButtonTypeCallback&>(*button->type_).data_);
            result.items.push_back(std::move(action));
          }
          else
          {
            result.text += button->text_ + ": requires Telegram (login, payment, password or Mini App).\n";
          }
        }
      }
      if (result.items.empty() && result.text.empty()) result.text = "No available buttons.";
      finish(std::move(result));
    });
    return true;
  }
  if (operation == "tg.button.callback")
  {
    const auto data = value("data");
    if (!msgId || data.empty() || (data.size() % 2) || data.size() > 128 ||
        data.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    {
      fail("This button's data is invalid; reopen the message buttons.");
      return true;
    }
    SendQuery(make_object<getCallbackQueryAnswer>(chatId, msgId, make_object<callbackQueryPayloadData>(StrUtil::StrFromHex(data))),
              [checked, finish, item](Object object)
    {
      if (!checked(object)) return;
      const auto& answer = static_cast<const callbackQueryAnswer&>(*object);
      ClientActionResult result;
      result.title = "Bot response";
      result.text = answer.text_.empty() ? "Button sent. The bot may update its message." : answer.text_;
      if (!answer.url_.empty())
      {
        auto link = item("Open response link", "openUrl", answer.url_);
        link.params["url"] = answer.url_;
        result.items.push_back(std::move(link));
      }
      finish(std::move(result));
    });
    return true;
  }




  fail("This Telegram chat action is not available.");
  return true;
}
