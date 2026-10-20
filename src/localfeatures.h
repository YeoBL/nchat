#pragma once

#include "fileutil.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Local-only preferences: none of these operations sends anything to a service.
// ponytail: one state file and snapshot per save; split stores only if it grows large.
class LocalFeatures
{
public:
  using Chat = std::pair<std::string, std::string>;
  struct Bookmark
  {
    Chat chat;
    std::string msgId;
    std::string text;
    int64_t time = 0;
  };

  LocalFeatures()
    : LocalFeatures(FileUtil::GetApplicationDir() + "/local-features/state")
  {
  }

  explicit LocalFeatures(const std::string& path)
    : m_Path(path)
  {
    Load();
  }

  const std::string& GetError() const { return m_Error; }
  bool IsWritable() const { return m_Writable; }

  bool Load()
  {
    m_Error.clear();
    const int fd = open(m_Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1)
    {
      if (errno == ENOENT)
      {
        m_State = State{};
        m_Writable = true;
        m_Dirty = false;
        return true;
      }
      m_Writable = false;
      return Fail("Cannot read local features", errno);
    }
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || static_cast<uint64_t>(info.st_size) > MaxBytes)
    {
      close(fd);
      m_Writable = false;
      return Fail("Invalid local features file");
    }
    std::string data(static_cast<size_t>(info.st_size), '\0');
    size_t offset = 0;
    while (offset < data.size())
    {
      const ssize_t count = read(fd, &data[offset], data.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0)
      {
        const int error = errno;
        close(fd);
        m_Writable = false;
        return Fail("Cannot read local features", count < 0 ? error : 0);
      }
      offset += static_cast<size_t>(count);
    }
    close(fd);
    State loaded;
    if (!Decode(data, loaded))
    {
      m_Writable = false;
      return Fail("Local features file is damaged; it has been left unchanged");
    }
    m_State = std::move(loaded);
    m_Writable = true;
    m_Dirty = false;
    return true;
  }

  std::string GetDraft(const Chat& chat) const
  {
    const auto it = m_State.drafts.find(chat);
    return it == m_State.drafts.end() ? "" : it->second;
  }

  // Defer keystroke updates, then Flush on an idle tick, chat switch, or shutdown.
  bool SetDraft(const Chat& chat, const std::string& text, bool persist = true)
  {
    if (GetDraft(chat) == text) return persist ? Flush() : Ready();
    if (!persist)
    {
      if (!Ready()) return false;
      if (text.empty()) m_State.drafts.erase(chat);
      else m_State.drafts[chat] = text;
      m_Dirty = true;
      return true;
    }
    return Change([&](State& state)
    {
      if (text.empty()) state.drafts.erase(chat);
      else state.drafts[chat] = text;
    });
  }

  bool Flush()
  {
    if (!Ready()) return false;
    if (!m_Dirty) return true;
    if (!Save(m_State)) return false;
    m_Dirty = false;
    return true;
  }

  std::vector<std::string> GetFolders() const
  {
    std::vector<std::string> result;
    for (const auto& folder : m_State.folders) result.push_back(folder.first);
    return result;
  }

  std::vector<std::string> GetChatFolders(const Chat& chat) const
  {
    std::vector<std::string> result;
    for (const auto& folder : m_State.folders)
      if (folder.second.count(chat)) result.push_back(folder.first);
    return result;
  }

  bool InFolder(const Chat& chat, const std::string& name) const
  {
    const auto it = m_State.folders.find(name);
    return it != m_State.folders.end() && it->second.count(chat);
  }

  std::set<Chat> GetFolderChats(const std::string& name) const
  {
    const auto it = m_State.folders.find(name);
    return it == m_State.folders.end() ? std::set<Chat>{} : it->second;
  }

  bool CreateFolder(const std::string& name)
  {
    if (name.empty()) return Fail("A folder needs a name");
    if (m_State.folders.count(name)) return Ready();
    return Change([&](State& state) { state.folders[name]; });
  }

  bool DeleteFolder(const std::string& name)
  {
    if (!m_State.folders.count(name)) return Ready();
    return Change([&](State& state) { state.folders.erase(name); });
  }

  bool SetFolder(const Chat& chat, const std::string& name, bool include)
  {
    if (name.empty()) return Fail("A folder needs a name");
    if (InFolder(chat, name) == include) return Ready();
    return Change([&](State& state)
    {
      if (include) state.folders[name].insert(chat);
      else state.folders[name].erase(chat);
    });
  }

  bool IsBookmarked(const Chat& chat, const std::string& msgId) const
  {
    return m_State.bookmarks.count({chat, msgId});
  }

  std::vector<Bookmark> GetBookmarks() const
  {
    std::vector<Bookmark> result;
    for (const auto& item : m_State.bookmarks) result.push_back(item.second);
    return result;
  }

  bool SetBookmark(const Chat& chat, const std::string& msgId, const std::string& text, int64_t time = 0)
  {
    if (msgId.empty()) return Fail("A bookmark needs a message");
    return Change([&](State& state) { state.bookmarks[{chat, msgId}] = {chat, msgId, text, time}; });
  }

  bool RemoveBookmark(const Chat& chat, const std::string& msgId)
  {
    if (!IsBookmarked(chat, msgId)) return Ready();
    return Change([&](State& state) { state.bookmarks.erase({chat, msgId}); });
  }

private:
  struct State
  {
    std::map<Chat, std::string> drafts;
    std::map<std::string, std::set<Chat>> folders;
    std::map<std::pair<Chat, std::string>, Bookmark> bookmarks;
  };
  static constexpr uint32_t MaxRecords = 100000;
  static constexpr uint32_t MaxBytes = 16 * 1024 * 1024;

  bool Ready()
  {
    if (!m_Writable) return false;
    m_Error.clear();
    return true;
  }

  bool Fail(const std::string& message, int error = 0)
  {
    m_Error = message + (error ? ": " + std::string(strerror(error)) : "");
    return false;
  }

  template<typename Edit> bool Change(Edit edit)
  {
    if (!Ready()) return false;
    State next = m_State;
    edit(next);
    if (!Save(next)) return false;
    m_State = std::move(next);
    m_Dirty = false;
    return true;
  }

  static void Number(std::string& data, uint32_t value)
  {
    for (int shift = 24; shift >= 0; shift -= 8) data.push_back(static_cast<char>(value >> shift));
  }

  static bool Number(const std::string& data, size_t& pos, uint32_t& value)
  {
    if (data.size() - pos < 4) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | static_cast<unsigned char>(data[pos++]);
    return true;
  }

  static void String(std::string& data, const std::string& value)
  {
    if (data.size() > MaxBytes || value.size() > MaxBytes || value.size() + 4 > MaxBytes - data.size())
    {
      data.resize(MaxBytes + 1);
      return;
    }
    Number(data, static_cast<uint32_t>(value.size()));
    data += value;
  }

  static bool String(const std::string& data, size_t& pos, std::string& value)
  {
    uint32_t size = 0;
    if (!Number(data, pos, size) || size > data.size() - pos) return false;
    value = data.substr(pos, size);
    pos += size;
    return true;
  }

  static bool Decode(const std::string& data, State& state)
  {
    if (data.compare(0, 8, "NCHATLF1") != 0) return false;
    size_t pos = 8;
    uint32_t count = 0;
    if (!Number(data, pos, count) || count > MaxRecords) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
      Chat chat;
      std::string text;
      if (!String(data, pos, chat.first) || !String(data, pos, chat.second) ||
          !String(data, pos, text) || text.empty() || !state.drafts.emplace(chat, text).second) return false;
    }
    if (!Number(data, pos, count) || count > MaxRecords) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
      std::string name;
      uint32_t size = 0;
      if (!String(data, pos, name) || name.empty() || state.folders.count(name) ||
          !Number(data, pos, size) || size > MaxRecords) return false;
      auto& chats = state.folders[name];
      for (uint32_t j = 0; j < size; ++j)
      {
        Chat chat;
        if (!String(data, pos, chat.first) || !String(data, pos, chat.second) ||
            !chats.insert(chat).second) return false;
      }
    }
    if (!Number(data, pos, count) || count > MaxRecords) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
      Bookmark bookmark;
      uint32_t hi = 0, lo = 0;
      if (!String(data, pos, bookmark.chat.first) || !String(data, pos, bookmark.chat.second) ||
          !String(data, pos, bookmark.msgId) || bookmark.msgId.empty() || !String(data, pos, bookmark.text) ||
          !Number(data, pos, hi) || !Number(data, pos, lo)) return false;
      bookmark.time = static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
      if (!state.bookmarks.emplace(std::make_pair(bookmark.chat, bookmark.msgId), bookmark).second) return false;
    }
    return pos == data.size();
  }

  bool Save(const State& state)
  {
    if (state.drafts.size() > MaxRecords || state.folders.size() > MaxRecords ||
        state.bookmarks.size() > MaxRecords) return Fail("Too many local feature records");
    std::string data = "NCHATLF1";
    Number(data, static_cast<uint32_t>(state.drafts.size()));
    for (const auto& item : state.drafts)
    {
      String(data, item.first.first);
      String(data, item.first.second);
      String(data, item.second);
    }
    Number(data, static_cast<uint32_t>(state.folders.size()));
    for (const auto& item : state.folders)
    {
      if (item.second.size() > MaxRecords) return Fail("Too many chats in a folder");
      String(data, item.first);
      Number(data, static_cast<uint32_t>(item.second.size()));
      for (const auto& chat : item.second)
      {
        String(data, chat.first);
        String(data, chat.second);
      }
    }
    Number(data, static_cast<uint32_t>(state.bookmarks.size()));
    for (const auto& item : state.bookmarks)
    {
      const Bookmark& bookmark = item.second;
      String(data, bookmark.chat.first);
      String(data, bookmark.chat.second);
      String(data, bookmark.msgId);
      String(data, bookmark.text);
      Number(data, static_cast<uint32_t>(static_cast<uint64_t>(bookmark.time) >> 32));
      Number(data, static_cast<uint32_t>(bookmark.time));
    }
    if (data.size() > MaxBytes) return Fail("Local features exceed the 16 MiB limit");
    const size_t slash = m_Path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." : (slash == 0 ? "/" : m_Path.substr(0, slash));
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
      return Fail("Cannot create local features directory", errno);
    std::string temporary = m_Path + ".XXXXXX";
    std::vector<char> path(temporary.begin(), temporary.end());
    path.push_back('\0');
    const int fd = mkstemp(path.data()); // Creates a private file beside the destination.
    if (fd < 0) return Fail("Cannot create local features file", errno);
    size_t written = 0;
    int error = 0;
    while (written < data.size())
    {
      const ssize_t size = write(fd, data.data() + written, data.size() - written);
      if (size < 0 && errno == EINTR) continue;
      if (size <= 0) { error = size < 0 ? errno : EIO; break; }
      written += static_cast<size_t>(size);
    }
    if (!error && fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && !error) error = errno;
    if (!error && rename(path.data(), m_Path.c_str()) != 0) error = errno;
    if (error)
    {
      unlink(path.data());
      return Fail("Cannot save local features", error);
    }
    const int dirFd = open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (dirFd >= 0) { fsync(dirFd); close(dirFd); }
    return true;
  }

  std::string m_Path;
  std::string m_Error;
  State m_State;
  bool m_Writable = false;
  bool m_Dirty = false;
};
