// Small input helpers shared by the Telegram account tools and their check.
#pragma once

#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

inline bool TgFeatureParseNumber(const std::string& value, int64_t& number)
{
  if (value.empty()) return false;
  try
  {
    size_t consumed = 0;
    number = std::stoll(value, &consumed);
    return consumed == value.size();
  }
  catch (...) { return false; }
}

inline bool TgFeatureParseDate(const std::string& value, int32_t& timestamp, bool endOfDay = false)
{
  if (value.empty()) { timestamp = 0; return true; }
  std::tm date{};
  std::istringstream stream(value);
  stream >> std::get_time(&date, "%Y-%m-%d");
  if (stream.fail() || stream.peek() != EOF || value.size() != 10) return false;
  const int year = date.tm_year, month = date.tm_mon, day = date.tm_mday;
  date.tm_hour = endOfDay ? 23 : 0;
  date.tm_min = endOfDay ? 59 : 0;
  date.tm_sec = endOfDay ? 59 : 0;
  date.tm_isdst = -1;
  const std::time_t parsed = std::mktime(&date);
  if (parsed < 0 || parsed > INT32_MAX || date.tm_year != year || date.tm_mon != month || date.tm_mday != day)
    return false;
  timestamp = static_cast<int32_t>(parsed);
  return true;
}

inline bool TgFeaturePollPageHasMore(int offset, int count, int total)
{
  return count > 0 && offset + count < total;
}
