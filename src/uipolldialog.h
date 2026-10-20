#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "strutil.h"
#include "protocol.h"
#include "uilistdialog.h"
#include "uimodel.h"

// Poll options are displayed in the message as separate bullet lines. The
// protocol validates the selected index before submitting the vote.
class UiPollDialog : public UiListDialog
{
  struct State
  {
    std::mutex mutex;
    bool done = false;
    std::string result;
  };
  std::shared_ptr<State> m_State = std::make_shared<State>();
  struct VotesState
  {
    std::mutex mutex;
    bool ready = false;
    PollVoteInfo info;
  };
  std::shared_ptr<VotesState> m_Votes = std::make_shared<VotesState>();
  std::shared_ptr<Protocol> m_Protocol;
  std::shared_ptr<VotePollRequest> m_Request;
  std::string m_Profile;
  std::string m_Question;
  std::vector<std::string> m_Options;
  std::vector<int> m_Visible;
  std::set<int> m_Selected;
  int m_MaxChoices = 1;
  bool m_Submitted = false;
  bool m_DoneDisplayed = false;
  bool m_VotesDisplayed = false;
  std::chrono::steady_clock::time_point m_Started = std::chrono::steady_clock::now();

public:
  UiPollDialog(const UiDialogParams& params, const std::shared_ptr<Protocol>& protocol,
               const std::shared_ptr<VotePollRequest>& request, const std::string& profile,
               const std::string& text)
    : UiListDialog(params, false), m_Protocol(protocol), m_Request(request), m_Profile(profile)
  {
    const auto firstEnd = text.find('\n');
    m_Question = text.substr(7, firstEnd == std::string::npos ? std::string::npos : firstEnd - 7);
    size_t start = firstEnd;
    while (start != std::string::npos)
    {
      ++start;
      auto end = text.find('\n', start);
      const auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (line.rfind("• ", 0) == 0) m_Options.push_back(line.substr(std::string("• ").size()));
      start = end;
    }
    if (text.find("\n[Choose multiple]") != std::string::npos)
      m_MaxChoices = static_cast<int>(m_Options.size());
    else
    {
      const auto marker = text.find("\n[Choose up to ");
      if (marker != std::string::npos)
      {
        try { m_MaxChoices = std::max(1, std::min(std::stoi(text.substr(marker + 15)),
          static_cast<int>(m_Options.size()))); } catch (...) { m_MaxChoices = 1; }
      }
    }
    UpdateList();
    auto votesRequest = std::make_shared<GetPollVotesRequest>();
    votesRequest->chatId = request->chatId;
    votesRequest->msgId = request->msgId;
    std::weak_ptr<VotesState> weak = m_Votes;
    votesRequest->onResult = [weak](PollVoteInfo info)
    {
      if (auto state = weak.lock())
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->info = std::move(info);
        state->ready = true;
      }
    };
    protocol->SendRequest(votesRequest);
  }

protected:
  void OnSelect() override
  {
    if (m_Submitted || m_Index < 0 || m_Index >= static_cast<int>(m_Visible.size())) return;
    const int option = m_Visible[m_Index];
    if (option == -2 && !m_Selected.empty()) { SubmitVote(); return; }
    if (option < 0) return;
    if (m_MaxChoices > 1)
    {
      if (m_Selected.count(option)) m_Selected.erase(option);
      else if (static_cast<int>(m_Selected.size()) < m_MaxChoices) m_Selected.insert(option);
      UpdateList();
      return;
    }
    m_Selected = {option};
    SubmitVote();
  }
  void SubmitVote()
  {
    m_Request->optionIndexes.assign(m_Selected.begin(), m_Selected.end());
    m_Submitted = true;
    std::weak_ptr<State> weak = m_State;
    m_Request->onResult = [weak](bool success, std::string message)
    {
      if (auto state = weak.lock())
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->result = success ? "Vote sent. Close to return." : std::move(message);
        state->done = true;
      }
    };
    m_Protocol->SendRequest(m_Request);
    UpdateList();
  }
  void OnBack() override { m_Running = false; }
  bool OnTimer() override
  {
    bool done;
    { std::lock_guard<std::mutex> lock(m_State->mutex); done = m_State->done; }
    if (done && !m_DoneDisplayed) { UpdateList(); m_DoneDisplayed = true; return true; }
    bool votesReady;
    {
      std::lock_guard<std::mutex> lock(m_Votes->mutex);
      if (!m_Votes->ready && std::chrono::steady_clock::now() - m_Started > std::chrono::seconds(15))
      {
        m_Votes->info.note = "Voter request timed out; reopen when connected.";
        m_Votes->ready = true;
      }
      votesReady = m_Votes->ready;
    }
    if (votesReady && !m_VotesDisplayed) { UpdateList(); m_VotesDisplayed = true; return true; }
    return false;
  }
  void UpdateList() override
  {
    m_Items.clear();
    m_Visible.clear();
    auto add = [&](const std::string& line, int option)
    {
      m_Items.push_back(StrUtil::TrimPadWString(StrUtil::ToWString(line), m_W));
      m_Visible.push_back(option);
    };
    add(m_Question, -1);
    std::string result;
    { std::lock_guard<std::mutex> lock(m_State->mutex); result = m_State->result; }
    if (!result.empty()) add(result, -1);
    else if (m_Submitted) add("Submitting vote...", -1);
    else add(m_MaxChoices > 1 ? "Enter toggles choices; select Submit to vote:" :
      "Select an option and press Enter to vote:", -1);
    PollVoteInfo voteInfo;
    bool votesReady;
    {
      std::lock_guard<std::mutex> lock(m_Votes->mutex);
      voteInfo = m_Votes->info;
      votesReady = m_Votes->ready;
    }
    const auto filter = StrUtil::ToLower(StrUtil::ToString(m_FilterStr));
    for (size_t i = 0; i < m_Options.size(); ++i)
      if (filter.empty() || StrUtil::ToLower(m_Options[i]).find(filter) != std::string::npos)
      {
        add((m_MaxChoices > 1 ? (m_Selected.count(static_cast<int>(i)) ? "[x] " : "[ ] ") : "  ") +
          m_Options[i], static_cast<int>(i));
        if (votesReady)
          for (const auto& vote : voteInfo.votes)
            if (vote.optionIndex == static_cast<int>(i))
            {
              std::string name = vote.userId == s_ReactionsSelfId ? "You" :
                m_Model->GetContactListName(m_Profile, vote.userId, true, true);
              if (name.empty()) name = vote.userId;
              add("    " + name, -1);
            }
      }
    if (m_MaxChoices > 1 && !m_Submitted) add("Submit vote (" +
      std::to_string(m_Selected.size()) + "/" + std::to_string(m_MaxChoices) + ")", -2);
    if (!votesReady) add("Loading voters...", -1);
    else if (!voteInfo.note.empty()) add(voteInfo.note, -1);
    m_Index = std::min(m_Index, std::max(0, static_cast<int>(m_Items.size()) - 1));
  }
};
