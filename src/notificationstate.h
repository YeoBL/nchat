#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <cstdint>

// Notification state is independent of the currently loaded chat history.
class NotificationState
{
public:
  using Chat = std::pair<std::string, std::string>;

  static bool IsMembershipEvent(const std::string& text)
  {
    return text == "[Joined]" || text == "[Left]" ||
      (text.size() > 8 && text.back() == ']' &&
       (text.compare(0, 7, "[Added ") == 0 || text.compare(0, 9, "[Removed ") == 0));
  }

  bool Add(const Chat& chat, const std::string& id, int64_t time = 0, bool hasMention = false)
  {
    if (IsRead(chat, id))
    {
      DismissThrough(chat, time); // A receipt may arrive before the message.
      return false;
    }
    if (time > 0 && time <= m_ReadTime[chat]) return false;
    if (hasMention) m_Mentions[chat].insert(id);
    if (!m_Seen[chat].insert(id).second) return false;
    m_Times[chat][id] = time;
    m_Pending[chat].insert(id);
    m_Ring = true;
    return true;
  }

  void Read(const Chat& chat, const std::string& id, const std::string& through = "", int64_t time = 0)
  {
    // Read progress in a chat retires earlier alerts, even when WhatsApp
    // omits a receipt for one old message. This changes notification state
    // only; it does not send receipts or rewrite the message cache.
    if (!id.empty() && through.empty())
    {
      const auto known = m_Times[chat].find(id);
      if (known != m_Times[chat].end() && known->second > time) time = known->second;
      DismissThrough(chat, time);
    }
    // Receipts can precede asynchronously loaded history. Retain them even
    // when no alert exists yet; otherwise stale cache results resurrect it.
    if (!through.empty())
    {
      auto& marker = m_ReadThrough[chat];
      if (marker.empty() || BeforeOrEqual(marker, through)) marker = through;
    }
    else if (!id.empty()) m_Read[chat].insert(id);
    auto it = m_Pending.find(chat);
    if (it == m_Pending.end()) return;
    if (!through.empty())
    {
      for (auto msg = it->second.begin(); msg != it->second.end(); )
      {
        // Telegram IDs are hex-encoded positive decimal strings.
        if (BeforeOrEqual(*msg, through))
          msg = it->second.erase(msg);
        else
          ++msg;
      }
    }
    else if (id.empty())
    {
      m_Read[chat].insert(it->second.begin(), it->second.end());
      it->second.clear();
    }
    else it->second.erase(id);
    if (it->second.empty()) m_Pending.erase(it);
    if (m_Pending.empty()) m_Ring = false; // Never ring after a queued read.
  }

  bool TakeBell()
  {
    const bool ring = m_Ring && !m_Pending.empty();
    m_Ring = false;
    if (ring) m_Badge = true;
    return ring;
  }

  void Remove(const Chat& chat, const std::string& id)
  {
    auto pending = m_Pending.find(chat);
    if (pending == m_Pending.end()) return;
    pending->second.erase(id);
    if (pending->second.empty()) m_Pending.erase(pending);
    if (m_Pending.empty()) m_Ring = false;
  }

  bool TakeClear()
  {
    if (!m_Badge || !m_Pending.empty()) return false;
    m_Badge = false;
    return true;
  }

  size_t PendingChats() const { return m_Pending.size(); }
  void RemoveChat(const Chat& chat, bool preserveMentions = false)
  {
    auto pending = m_Pending.find(chat);
    if (pending == m_Pending.end()) return;
    if (preserveMentions)
    {
      for (auto id = pending->second.begin(); id != pending->second.end(); )
        if (!m_Mentions[chat].count(*id)) id = pending->second.erase(id);
        else ++id;
    }
    else pending->second.clear();
    if (pending->second.empty()) m_Pending.erase(pending);
    if (m_Pending.empty()) m_Ring = false;
  }

  bool IsRead(const Chat& chat, const std::string& id) const
  {
    const auto read = m_Read.find(chat);
    if (read != m_Read.end() && read->second.count(id)) return true;
    const auto marker = m_ReadThrough.find(chat);
    return marker != m_ReadThrough.end() && BeforeOrEqual(id, marker->second);
  }

private:
  void DismissThrough(const Chat& chat, int64_t time)
  {
    // Telegram forum topics can be read independently and out of time order.
    if (chat.first.rfind("Telegram", 0) == 0) return;
    if (time <= 0) return;
    auto& progress = m_ReadTime[chat];
    if (time > progress) progress = time;
    auto pending = m_Pending.find(chat);
    if (pending == m_Pending.end()) return;
    for (auto msg = pending->second.begin(); msg != pending->second.end(); )
    {
      const auto known = m_Times[chat].find(*msg);
      if (known != m_Times[chat].end() && known->second > 0 && known->second <= progress)
        msg = pending->second.erase(msg);
      else
        ++msg;
    }
    if (pending->second.empty()) m_Pending.erase(pending);
    if (m_Pending.empty()) m_Ring = false;
  }

  std::map<Chat, std::map<std::string, int64_t>> m_Times;
  std::map<Chat, int64_t> m_ReadTime;
  static bool BeforeOrEqual(const std::string& id, const std::string& through)
  {
    return id.size() < through.size() || (id.size() == through.size() && id <= through);
  }
  std::map<Chat, std::set<std::string>> m_Read;
  std::map<Chat, std::string> m_ReadThrough;
  // Retain seen IDs for the session: cache reloads must not replay old alerts.
  std::map<Chat, std::set<std::string>> m_Seen;
  std::map<Chat, std::set<std::string>> m_Pending;
  std::map<Chat, std::set<std::string>> m_Mentions;
  bool m_Ring = false;
  bool m_Badge = false;
};
