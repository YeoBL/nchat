#include "uicursestext.h"

#include <map>
#include <chrono>
#include <vector>

#include "strutil.h"
#include "../lib/ncutil/ext/cereal/include/cereal/external/rapidjson/stringbuffer.h"
#include "../lib/ncutil/ext/cereal/include/cereal/external/rapidjson/writer.h"

namespace
{
  struct Placement
  {
    int y, x;
    std::wstring text;
    cchar_t marker;
  };
  std::map<WINDOW*, std::vector<Placement>> placements;
  std::map<std::pair<int, int>, Placement> visible;
  FILE* output = stdout;
  bool batching = false, dirty = false;
  auto lastPaint = std::chrono::steady_clock::now();

  bool Matches(WINDOW* window, int y, int x, const cchar_t& marker)
  {
    int savedY = 0, savedX = 0;
    getyx(window, savedY, savedX);
    cchar_t cell = {};
    const bool matches = mvwin_wch(window, y, x, &cell) != ERR &&
      (cell.attr & A_CHARTEXT) <= 1 && // Reject the continuation cell of a wide glyph.
      (cell.attr & ~A_CHARTEXT) == (marker.attr & ~A_CHARTEXT) &&
      std::equal(std::begin(cell.chars), std::end(cell.chars), std::begin(marker.chars));
    bool complete = matches;
    if (matches && wcwidth(marker.chars[0]) == 2)
    {
      cchar_t next = {};
      complete = mvwin_wch(window, y, x + 1, &next) != ERR &&
        (next.attr & A_CHARTEXT) == 2 &&
        std::equal(std::begin(next.chars), std::end(next.chars), std::begin(marker.chars));
    }
    wmove(window, savedY, savedX);
    return complete;
  }

  using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer, rapidjson::UTF8<>, rapidjson::ASCII<>>;

  // Screen's UTF-8 decoder corrupts non-ASCII DCS payloads. JSON's ASCII
  // escapes preserve the Unicode sequence. Split ST itself across two DCS
  // strings so it reaches Kitty instead of prematurely closing the relay.
  std::string Relay(const std::string& command)
  {
    if (!std::getenv("STY")) return command;
    std::string result;
    for (size_t offset = 0; offset < command.size() - 2; offset += 700)
      result += "\033P" + command.substr(offset, std::min(size_t(700), command.size() - 2 - offset)) + "\033\\";
    result += "\033P\033\033\\\033P\\\033\\";
    return result;
  }
}

void UiCursesText::Prepare(WINDOW* window, int y, int x, int width)
{
  auto& entries = placements[window];
  entries.erase(std::remove_if(entries.begin(), entries.end(), [=](const Placement& value) {
    return value.y == y && value.x < x + width && value.x + wcwidth(value.marker.chars[0]) > x;
  }), entries.end());
  const int row = getbegy(window) + y, left = getbegx(window) + x;
  for (auto it = visible.begin(); it != visible.end();)
  {
    if (it->first.first == row && it->first.second < left + width &&
        it->first.second + wcwidth(it->second.marker.chars[0]) > left) it = visible.erase(it);
    else ++it;
  }
}

void UiCursesText::Record(WINDOW* window, int y, int x, const std::wstring& text, const cchar_t& marker)
{
  auto& row = placements[window];
  row.erase(std::remove_if(row.begin(), row.end(), [=](const Placement& value) {
    return value.y == y && value.x == x;
  }), row.end());
  row.push_back({y, x, text, marker});
  dirty = true;
}

int UiCursesText::Erase(WINDOW* window)
{
  placements.erase(window);
  const int top = getbegy(window), left = getbegx(window);
  for (auto it = visible.begin(); it != visible.end();)
  {
    if (it->first.first >= top && it->first.first < top + getmaxy(window) &&
        it->first.second >= left && it->first.second < left + getmaxx(window)) it = visible.erase(it);
    else ++it;
  }
  return werase(window);
}

int UiCursesText::Delete(WINDOW* window)
{
  placements.erase(window);
  return delwin(window);
}

void UiCursesText::SetOutput(FILE* stream)
{
  output = stream;
  placements.clear();
  visible.clear();
  batching = false;
  dirty = false;
}

std::wstring UiCursesText::ScreenGlyph(int y, int x)
{
  auto found = visible.find({y, x});
  return found != visible.end() && Matches(curscr, y, x, found->second.marker) ? found->second.text : L"";
}

namespace
{
  void Flush()
  {
    const auto now = std::chrono::steady_clock::now();
    if (visible.empty() || (!dirty && now - lastPaint < std::chrono::seconds(1))) return;
    lastPaint = now;
    dirty = false;
    auto entry = visible.begin();
    while (entry != visible.end())
    {
      // Bound each helper call even for dense emoji on very large terminals.
      rapidjson::StringBuffer data;
      JsonWriter json(data);
      json.StartArray();
      size_t count = 0;
      while (entry != visible.end() && count < 128)
      {
        const int y = entry->first.first, x = entry->first.second;
        const auto& value = entry->second;
        if (!Matches(curscr, y, x, value.marker))
        {
          entry = visible.erase(entry);
          continue;
        }
        short fg = -1, bg = -1;
        pair_content(PAIR_NUMBER(value.marker.attr), &fg, &bg);
        json.StartArray();
        json.Int(y); json.Int(x); json.String(StrUtil::ToString(value.text).c_str());
        json.Int(TextGlyph::Next(value.text.c_str(), 0, value.text.size()).width);
        json.Int(fg); json.Int(bg);
        json.Bool(value.marker.attr & A_BOLD); json.Bool(value.marker.attr & A_DIM);
        json.Bool(value.marker.attr & A_REVERSE); json.Bool(value.marker.attr & A_UNDERLINE);
        json.EndArray();
        ++entry;
        ++count;
      }
      json.EndArray();
      if (count == 0) continue;
      rapidjson::StringBuffer request;
      JsonWriter message(request);
      message.StartObject();
      message.Key("cmd"); message.String("kitten");
      message.Key("version"); message.StartArray(); message.Int(0); message.Int(40); message.Int(0); message.EndArray();
      message.Key("no_response"); message.Bool(true);
      message.Key("payload"); message.StartObject();
      message.Key("kitten"); message.String(TextGlyph::HelperPath().c_str());
      message.Key("args"); message.StartArray(); message.String(data.GetString()); message.EndArray();
      message.EndObject(); message.EndObject();
      const auto bytes = Relay(std::string("\033P@kitty-cmd") + request.GetString() + "\033\\");
      std::fwrite(bytes.data(), 1, bytes.size(), output);
    }
    std::fflush(output);
  }
}

void UiCursesText::BeginFrame() { batching = true; }
void UiCursesText::EndFrame() { batching = false; Flush(); }

int UiCursesText::Refresh(WINDOW* window)
{
  if (is_wintouched(window) && !visible.empty()) dirty = true;
  const int status = wrefresh(window);
  const auto found = placements.find(window);
  if (status == ERR) return status;
  if (found != placements.end())
  {
    for (const auto& value : found->second)
    {
      if (!Matches(window, value.y, value.x, value.marker)) continue;
      const int y = getbegy(window) + value.y, x = getbegx(window) + value.x;
      if (Matches(curscr, y, x, value.marker)) visible[{y, x}] = value;
    }
  }
  if (!batching) Flush();
  return status;
}
