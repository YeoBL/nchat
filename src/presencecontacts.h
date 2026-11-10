#pragma once

#include "strutil.h"
#include "../lib/ncutil/ext/cereal/include/cereal/external/rapidjson/document.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace PresenceContacts
{
using Key = std::pair<std::string, std::string>;
using Link = std::pair<Key, Key>;
using Selection = std::map<std::string, std::vector<std::string>>;
struct Contact
{
  Key key;
  std::string phone;
  int64_t lastMessageTime = 0;
  bool hasChat = false;
};

inline std::string Service(const std::string& profile)
{
  if (profile.compare(0, 9, "Telegram_") == 0) return "Telegram";
  if (profile.compare(0, 11, "WhatsAppMd_") == 0) return "WhatsApp";
  return "";
}

inline bool EndsWith(const std::string& value, const std::string& suffix)
{
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool PrivateId(const Key& key)
{
  if (Service(key.first) == "WhatsApp") return EndsWith(key.second, "@s.whatsapp.net") || EndsWith(key.second, "@lid");
  if (Service(key.first) != "Telegram") return false;
  if (key.second.size() % 2 || key.second.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  const auto user = StrUtil::StrFromHex(key.second);
  return !user.empty() && user.find_first_not_of("0123456789") == std::string::npos &&
    user.find_first_not_of('0') != std::string::npos;
}

// Same complete-number rule as nchat-presence. Never infer a phone from LID digits.
inline std::string Phone(std::string value)
{
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return "";
  value = value.substr(first, value.find_last_not_of(" \t") - first + 1);
  if (value.find_first_not_of("+0123456789-(). \t") != std::string::npos ||
      (value.find('+') != std::string::npos && (value[0] != '+' || value.find('+', 1) != std::string::npos))) return "";
  std::string digits;
  for (char ch : value) if (ch >= '0' && ch <= '9') digits += ch;
  if (digits.compare(0, 2, "00") == 0) digits.erase(0, 2);
  return digits.size() >= 7 && digits.size() <= 15 && digits[0] != '0' ? digits : "";
}

// Read only the existing private display-link file. A malformed snapshot is
// ignored as a whole, matching the viewer's automatic-match fallback.
inline std::vector<Link> ReadLinks(const std::string& path, std::string& error)
{
  error.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
  {
    if (errno != ENOENT) error = "Cannot read presence contact links";
    return {};
  }
  struct stat info{};
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
      (info.st_mode & 077) || info.st_size < 0 || info.st_size > 1048576)
  {
    close(fd);
    error = "Presence contact links need a private regular file";
    return {};
  }
  std::string bytes(static_cast<size_t>(info.st_size), '\0');
  size_t offset = 0;
  while (offset < bytes.size())
  {
    const auto count = read(fd, &bytes[offset], bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += static_cast<size_t>(count);
  }
  close(fd);
  rapidjson::Document data;
  data.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(bytes.data(), bytes.size());
  if (offset != bytes.size() || data.HasParseError() || !data.IsObject() ||
      !data.HasMember("version") || !data["version"].IsInt() || data["version"].GetInt() != 1 ||
      !data.HasMember("links") || !data["links"].IsArray() || data["links"].Size() > 1000)
  {
    error = "Presence contact links have an invalid format";
    return {};
  }
  std::vector<Link> links;
  for (const auto& pair : data["links"].GetArray())
  {
    Link link;
    if (!pair.IsArray() || pair.Size() != 2) { error = "Invalid presence contact link"; return {}; }
    for (size_t i = 0; i < 2; ++i)
    {
      const auto& identity = pair[static_cast<rapidjson::SizeType>(i)];
      if (!identity.IsArray() || identity.Size() != 2) { error = "Invalid presence contact identity"; return {}; }
      Key key;
      for (size_t field = 0; field < 2; ++field)
      {
        const auto& value = identity[static_cast<rapidjson::SizeType>(field)];
        if (!value.IsString() || !value.GetStringLength() || value.GetStringLength() > 2048 ||
            StrUtil::ToWString(std::string(value.GetString(), value.GetStringLength())).size() > 512 ||
            std::string(value.GetString(), value.GetStringLength()).find('\0') != std::string::npos)
        { error = "Invalid presence contact identity"; return {}; }
        (field == 0 ? key.first : key.second).assign(value.GetString(), value.GetStringLength());
      }
      (i == 0 ? link.first : link.second) = key;
    }
    links.push_back(link);
  }
  return links;
}

inline Selection Select(std::vector<Contact> contacts, size_t count, const std::vector<Link>& links)
{
  std::sort(contacts.begin(), contacts.end(), [](const Contact& left, const Contact& right) {
    if (left.key.first != right.key.first) return left.key.first < right.key.first;
    if (left.lastMessageTime != right.lastMessageTime) return left.lastMessageTime > right.lastMessageTime;
    return left.key.second < right.key.second;
  });
  std::map<Key, Key> parents;
  for (const auto& contact : contacts) parents[contact.key] = contact.key;
  for (const auto& link : links)
    for (const auto& key : {link.first, link.second}) parents.emplace(key, key);
  auto root = [&](Key key) {
    while (parents.at(key) != key)
    {
      parents[key] = parents.at(parents.at(key));
      key = parents.at(key);
    }
    return key;
  };
  std::map<std::string, Key> known;
  for (const auto& contact : contacts)
  {
    const auto service = Service(contact.key.first);
    std::vector<std::string> tokens;
    if (!service.empty()) tokens.push_back(service + ":" + contact.key.second);
    const auto phone = Phone(contact.phone);
    if (!service.empty() && !phone.empty()) tokens.push_back("phone:" + phone);
    for (const auto& token : tokens)
    {
      const auto previous = known.emplace(token, contact.key);
      if (!previous.second) parents[root(contact.key)] = root(previous.first->second);
    }
  }
  for (const auto& link : links) parents[root(link.second)] = root(link.first);
  std::map<Key, int64_t> selected;
  std::map<std::string, size_t> ranks;
  for (const auto& contact : contacts)
    if (contact.hasChat && ranks[contact.key.first]++ < count)
    {
      const auto key = root(contact.key);
      selected[key] = std::max(selected[key], contact.lastMessageTime);
    }
  std::stable_sort(contacts.begin(), contacts.end(), [&](const Contact& left, const Contact& right) {
    const auto a = selected.find(root(left.key)), b = selected.find(root(right.key));
    const auto timeA = a == selected.end() ? 0 : a->second;
    const auto timeB = b == selected.end() ? 0 : b->second;
    return timeA > timeB;
  });
  Selection result;
  for (const auto& contact : contacts)
    if (selected.count(root(contact.key))) result[contact.key.first].push_back(contact.key.second);
  return result;
}
}
