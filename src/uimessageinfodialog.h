#pragma once

#include <chrono>
#include <mutex>
#include "uilistdialog.h"
#include "uimodel.h"
#include "strutil.h"
#include "timeutil.h"

// Uses the existing scroll/filter dialog. Requests never open chats or mark read.
class UiMessageInfoDialog : public UiListDialog
{
  struct State
  {
    std::mutex mutex;
    bool ready = false;
    uint64_t generation = 0;
    uint64_t revision = 0;
    MessageReceiptInfo info;
  };
  std::shared_ptr<State> m_State = std::make_shared<State>();
  std::string m_Profile;
  std::shared_ptr<Protocol> m_Protocol;
  std::shared_ptr<GetMessageReceiptsRequest> m_Request;
  bool m_Displayed = false;
  uint64_t m_DisplayedRevision = 0;
  int m_RefreshIndex = -1;
  std::chrono::steady_clock::time_point m_Started = std::chrono::steady_clock::now();

  static std::string UnavailableReason(const std::string& note)
  {
    const auto lower = StrUtil::ToLower(note);
    if (lower.find("timed out") != std::string::npos) return "Request timed out. Retry.";
    if (lower.find("not been read") != std::string::npos || lower.find("unread") != std::string::npos)
      return "Not read yet.";
    if (lower.find("too old") != std::string::npos) return "Read details have expired.";
    if (lower.find("eligibility") != std::string::npos || lower.find("chat type") != std::string::npos ||
        lower.find("message age") != std::string::npos)
      return "Unavailable: message age, chat type or privacy limits.";
    if (lower.find("privacy") != std::string::npos) return "Unavailable due to privacy settings.";
    if (lower.find("unavailable") != std::string::npos || lower.find("connected") != std::string::npos ||
        lower.find("error") != std::string::npos || lower.find("failed") != std::string::npos)
      return "Service unavailable. Retry when connected.";
    return "";
  }

public:
  UiMessageInfoDialog(const UiDialogParams& params, const std::string& profile,
    const std::shared_ptr<Protocol>& protocol, const std::shared_ptr<GetMessageReceiptsRequest>& request)
    : UiListDialog(params, false), m_Profile(profile), m_Protocol(protocol),
      m_Request(std::make_shared<GetMessageReceiptsRequest>(*request))
  {
    RequestDetails();
  }

protected:
  void RequestDetails()
  {
    uint64_t generation;
    {
      std::lock_guard<std::mutex> lock(m_State->mutex);
      generation = ++m_State->generation;
      ++m_State->revision;
      m_State->ready = false;
      m_State->info = {};
    }
    m_Started = std::chrono::steady_clock::now();
    auto request = std::make_shared<GetMessageReceiptsRequest>(*m_Request);
    std::weak_ptr<State> weak = m_State;
    request->onResult = [weak, generation](MessageReceiptInfo info)
    {
      if (auto state = weak.lock())
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->generation != generation) return;
        state->info = std::move(info);
        state->ready = true;
        ++state->revision;
      }
    };
    UpdateList();
    m_Protocol->SendRequest(request);
  }

  void OnSelect() override
  {
    if (m_Index == m_RefreshIndex) RequestDetails();
    else m_Running = false;
  }
  void OnBack() override { m_Running = false; }
  bool OnTimer() override
  {
    bool changed;
    {
      std::lock_guard<std::mutex> lock(m_State->mutex);
      if (!m_State->ready && std::chrono::steady_clock::now() - m_Started > std::chrono::seconds(15))
      {
        m_State->info.note = "Receipt request timed out. Retry when connected.";
        m_State->ready = true;
        ++m_State->generation; // A late response must not replace this retryable error.
        ++m_State->revision;
      }
      changed = m_State->revision != m_DisplayedRevision;
    }
    if (changed) { UpdateList(); return true; }
    return false;
  }
  void UpdateList() override
  {
    MessageReceiptInfo info;
    {
      std::lock_guard<std::mutex> lock(m_State->mutex);
      m_Displayed = m_State->ready;
      m_DisplayedRevision = m_State->revision;
      info = m_State->info;
    }
    std::vector<std::string> lines;
    if (!m_Displayed) lines.push_back("Loading receipt details... (Ctrl-C to cancel)");
    else
    {
      const bool telegram = m_Profile.rfind("Telegram", 0) == 0;
      const auto unavailable = UnavailableReason(info.note);
      auto addPeople = [&](bool read)
      {
        std::vector<std::string> people;
        for (const auto& receipt : info.receipts)
        {
          const int64_t timestamp = read ? receipt.readTime : receipt.deliveredTime;
          if (read ? (timestamp <= 0 && !(telegram && timestamp == -1))
                   : (timestamp <= 0 && receipt.readTime <= 0)) continue;
          std::string name = m_Model->GetContactListName(m_Profile, receipt.userId, true, true);
          if (name.empty()) name = receipt.userId;
          people.push_back(name + "  -  " + (timestamp > 0 ? TimeUtil::GetTimeString(timestamp * 1000, true) :
            (read ? "time unavailable" : "delivery time unavailable (read confirmed)")));
        }
        // An absent Telegram reader list is not evidence that nobody read it.
        const auto count = telegram && people.empty() && unavailable != "Not read yet." ?
          "unavailable" : std::to_string(people.size());
        lines.push_back(std::string(read ? "Read by" : "Delivered to") + " - " + count);
        if (people.empty()) lines.push_back("  " + (!unavailable.empty() ? unavailable :
          (telegram ? "No reader details available." : "No receipts recorded.")));
        else for (const auto& person : people) lines.push_back("  " + person);
        return people.size();
      };
      const auto readers = addPeople(true);
      if (!telegram)
      {
        addPeople(false);
        lines.push_back("Recorded receipts; history may be incomplete.");
      }
      else if (readers > 0 && !unavailable.empty()) lines.push_back(unavailable);
    }
    m_Items.clear();
    const auto filter = StrUtil::ToLower(StrUtil::ToString(m_FilterStr));
    for (auto line : lines)
    {
      for (auto& c : line) if ((unsigned char)c < 32 || c == 127) c = ' ';
      if (!filter.empty() && StrUtil::ToLower(line).find(filter) == std::string::npos) continue;
      for (auto wrapped : StrUtil::WordWrap(StrUtil::ToWString(line), std::max(1, m_W), false, false, false, 2))
        m_Items.push_back(StrUtil::TrimPadWString(wrapped, m_W));
    }
    // This only re-queries the service/cache; it never opens a chat, marks it
    // read, or claims to retrieve a complete historical WhatsApp receipt list.
    m_RefreshIndex = static_cast<int>(m_Items.size());
    m_Items.push_back(StrUtil::TrimPadWString(L"[ Refresh receipts ]", m_W));
    m_Index = std::min(m_Index, std::max(0, (int)m_Items.size() - 1));
  }
};
