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









  fail("This Telegram action is not supported.");
  return true;
}
