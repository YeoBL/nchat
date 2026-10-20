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


  if (operation == "tg.send.silent" || operation == "tg.send.scheduled" || operation == "tg.button.reply")
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






  fail("This Telegram chat action is not available.");
  return true;
}
