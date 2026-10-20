#pragma once

#include <ctime>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

namespace FeatureUtil
{
inline std::string Value(const std::map<std::string, std::string>& values, const std::string& key)
{
  const auto found = values.find(key);
  return found == values.end() ? "" : found->second;
}

// Date bounds follow the user's local calendar; a day may be 23 or 25 hours.
inline bool DateBound(const std::string& value, bool end, int64_t& result)
{
  result = 0;
  if (value.empty()) return true;
  std::tm date{};
  std::istringstream input(value);
  input >> std::get_time(&date, "%Y-%m-%d");
  if (input.fail() || !input.eof() || value.size() != 10) return false;
  date.tm_isdst = -1;
  const std::time_t start = std::mktime(&date);
  char check[11];
  if (start == -1 || !std::strftime(check, sizeof(check), "%Y-%m-%d", &date) || value != check) return false;
  if (end)
  {
    ++date.tm_mday;
    date.tm_isdst = -1;
    result = static_cast<int64_t>(std::mktime(&date)) * 1000 - 1;
  }
  else result = static_cast<int64_t>(start) * 1000;
  return true;
}

inline bool SafeUrl(const std::string& value)
{
  if (value.find_first_of("\r\n\t") != std::string::npos) return false;
  return value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0 || value.rfind("tg://", 0) == 0;
}
}
