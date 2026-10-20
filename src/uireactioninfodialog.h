#pragma once

#include <chrono>
#include <mutex>
#include "uilistdialog.h"
#include "uimodel.h"
#include "strutil.h"

// Inspecting reactions never opens the chat or sends a read receipt.
class UiReactionInfoDialog : public UiListDialog
{
  struct State
  {
    std::mutex mutex;
    bool ready = false;
    MessageReactorInfo info;
  };
  std::shared_ptr<State> m_State = std::make_shared<State>();
  std::string m_Profile;
  bool m_Displayed = false;
  std::chrono::steady_clock::time_point m_Started = std::chrono::steady_clock::now();

public:
  UiReactionInfoDialog(const UiDialogParams& params, const std::string& profile,
    const Reactions& cached, const std::shared_ptr<Protocol>& protocol,
    const std::shared_ptr<GetMessageReactionsRequest>& request)
    : UiListDialog(params, false), m_Profile(profile)
  {
    if (profile.rfind("Telegram", 0) == 0 && protocol)
    {
      std::weak_ptr<State> weak = m_State;
      request->onResult = [weak](MessageReactorInfo info)
      {
        if (auto state = weak.lock())
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->info = std::move(info);
          state->ready = true;
        }
      };
      protocol->SendRequest(request);
    }
    else
    {
      m_State->ready = true;
      for (const auto& item : cached.senderEmojis)
        m_State->info.reactors.push_back({item.first, item.second});
      size_t total = 0;
      for (const auto& item : cached.emojiCounts) total += item.second;
      if (total > m_State->info.reactors.size())
        m_State->info.note = std::to_string(total - m_State->info.reactors.size()) +
          " more reaction(s) have no sender details in the local cache.";
    }
    UpdateList();
  }

protected:
  void OnSelect() override { m_Running = false; }
  void OnBack() override { m_Running = false; }
  bool OnTimer() override
  {
    bool ready;
    {
      std::lock_guard<std::mutex> lock(m_State->mutex);
      if (!m_State->ready && std::chrono::steady_clock::now() - m_Started > std::chrono::seconds(15))
      {
        m_State->info.note = "Reaction request timed out. Close and retry when connected.";
        m_State->ready = true;
      }
      ready = m_State->ready;
    }
    if (ready && !m_Displayed) { UpdateList(); return true; }
    return false;
  }
  void UpdateList() override
  {
    MessageReactorInfo info;
    {
      std::lock_guard<std::mutex> lock(m_State->mutex);
      m_Displayed = m_State->ready;
      info = m_State->info;
    }
    std::vector<std::string> lines;
    if (!m_Displayed) lines.push_back("Loading reactions... (Ctrl-C to cancel)");
    else
    {
      lines.push_back("Reacted by - " + std::to_string(info.reactors.size()));
      for (const auto& reactor : info.reactors)
      {
        std::string name = reactor.userId == s_ReactionsSelfId ? "You" :
          m_Model->GetContactListName(m_Profile, reactor.userId, true, true);
        if (name.empty()) name = reactor.userId;
        lines.push_back("  " + name + "  -  " + reactor.emoji);
      }
      if (info.reactors.empty()) lines.push_back("  No reactor names available.");
      if (!info.note.empty()) lines.push_back(info.note);
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
    m_Index = std::min(m_Index, std::max(0, (int)m_Items.size() - 1));
  }
};
