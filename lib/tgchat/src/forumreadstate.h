#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

// Forum topics have independent inbox cursors, not one cursor for the chat.
class ForumReadState
{
public:
  using Key = std::pair<int64_t, int32_t>;
  bool Track(Key key, int64_t message)
  {
    if (message <= cursors[key]) return true;
    pending[key].insert(message);
    return false;
  }
  std::vector<int64_t> Read(Key key, int64_t through)
  {
    auto& cursor = cursors[key];
    cursor = std::max(cursor, through);
    auto& messages = pending[key];
    auto end = messages.upper_bound(cursor);
    std::vector<int64_t> result(messages.begin(), end);
    messages.erase(messages.begin(), end);
    return result;
  }
private:
  std::map<Key, int64_t> cursors;
  std::map<Key, std::set<int64_t>> pending;
};
