#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <utility>
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
    bool success = false;
    unsigned generation = 0;
    std::string result;
  };
  std::shared_ptr<State> m_State = std::make_shared<State>();
  struct VotesState
  {
    std::mutex mutex;
    bool ready = false;
    unsigned revision = 0;
    unsigned generation = 0;
    PollVoteInfo info;
  };
  std::shared_ptr<VotesState> m_Votes = std::make_shared<VotesState>();
  std::shared_ptr<Protocol> m_Protocol;
  std::shared_ptr<VotePollRequest> m_Request;
  std::string m_Profile;
  std::string m_Question;
  std::vector<std::string> m_Options;
  std::vector<int> m_Visible;
  std::vector<std::string> m_RowKeys;
  std::set<int> m_Selected;
  int m_MaxChoices = 1;
  bool m_Submitted = false;
  bool m_DoneDisplayed = false;
  bool m_VotesDisplayed = false;
  unsigned m_VotesRevision = 0;
  bool m_SelectionTouched = false;
  bool m_AwaitingRefresh = false;
  bool m_Unconfirmed = false;
  std::chrono::steady_clock::time_point m_Started = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point m_SubmitStarted = std::chrono::steady_clock::now();

public:
  UiPollDialog(const UiDialogParams& params, const std::shared_ptr<Protocol>& protocol,
               const std::shared_ptr<VotePollRequest>& request, const std::string& profile,
               const std::string& text)
    : UiListDialog(params, false), m_Protocol(protocol), m_Request(request), m_Profile(profile)
  {
    const auto optionsStart = text.find("\n• ");
    m_Question = text.substr(7, optionsStart == std::string::npos ? std::string::npos : optionsStart - 7);
    size_t start = optionsStart;
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
    RefreshVotes();
  }

protected:
  void RefreshVotes()
  {
    m_Started = std::chrono::steady_clock::now();
    m_VotesDisplayed = false;
    unsigned generation;
    {
      std::lock_guard<std::mutex> lock(m_Votes->mutex);
      m_Votes->ready = false;
      generation = ++m_Votes->generation;
    }
    auto votesRequest = std::make_shared<GetPollVotesRequest>();
    votesRequest->chatId = m_Request->chatId;
    votesRequest->msgId = m_Request->msgId;
    std::weak_ptr<VotesState> weak = m_Votes;
    votesRequest->isCancelled = [weak, generation]()
    {
      const auto state = weak.lock();
      if (!state) return true;
      std::lock_guard<std::mutex> lock(state->mutex);
      return state->generation != generation;
    };
    votesRequest->onResult = [weak, generation](PollVoteInfo info)
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
    m_Protocol->SendRequest(votesRequest);
  }

  void OnSelect() override
  {
    if (m_Submitted || m_Index < 0 || m_Index >= static_cast<int>(m_Visible.size())) return;
    const int option = m_Visible[m_Index];
    PollVoteInfo info;
    bool ready;
    { std::lock_guard<std::mutex> lock(m_Votes->mutex); info = m_Votes->info; ready = m_Votes->ready; }
    if (!ready) return;
    if (option == -4)
    {
      m_Unconfirmed = false;
      m_SelectionTouched = false;
      RefreshVotes(); UpdateList(); return;
    }
    if (!info.canVote || m_Unconfirmed) return;
    if (option == -2 && (!m_Selected.empty() ||
        (info.canRetract && !info.selectedOptionIndexes.empty()))) { SubmitVote(); return; }
    if (option == -3) { m_Selected.clear(); m_SelectionTouched = true; UpdateList(); return; }
    if (option < 0) return;
    m_SelectionTouched = true;
    if (m_Selected.count(option)) m_Selected.erase(option);
    else if (m_MaxChoices == 1) m_Selected = {option};
    else if (static_cast<int>(m_Selected.size()) < m_MaxChoices) m_Selected.insert(option);
    UpdateList();
  }
  void SubmitVote()
  {
    auto request = std::make_shared<VotePollRequest>(*m_Request);
    request->optionIndexes.assign(m_Selected.begin(), m_Selected.end());
    m_Submitted = true;
    m_SubmitStarted = std::chrono::steady_clock::now();
    m_DoneDisplayed = false;
    unsigned generation;
    {
      std::lock_guard<std::mutex> lock(m_State->mutex);
      m_State->done = false; m_State->result.clear();
      generation = ++m_State->generation;
    }
    std::weak_ptr<State> weak = m_State;
    request->onResult = [weak, generation](bool success, std::string message)
    {
      if (auto state = weak.lock())
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->generation != generation) return;
        state->result = success ? "Vote saved. Refreshing results..." : std::move(message);
        state->success = success;
        state->done = true;
      }
    };
    m_Protocol->SendRequest(request);
    UpdateList();
  }
  void OnBack() override { m_Running = false; }
  bool OnTimer() override
  {
    bool done, success;
    bool timedOut = false;
    {
      std::lock_guard<std::mutex> lock(m_State->mutex);
      if (m_Submitted && !m_AwaitingRefresh && !m_State->done &&
          std::chrono::steady_clock::now() - m_SubmitStarted > std::chrono::seconds(30))
      {
        ++m_State->generation; // A late acknowledgement must not confirm a later retry.
        m_State->result = "Vote unconfirmed. Select Refresh results to check; no vote was retried.";
        m_Submitted = false;
        m_Unconfirmed = true;
        timedOut = true;
      }
      done = m_State->done; success = m_State->success;
    }
    if (timedOut) { UpdateList(); return true; }
    if (done && !m_DoneDisplayed)
    {
      m_DoneDisplayed = true;
      if (success) { m_AwaitingRefresh = true; m_SelectionTouched = false; RefreshVotes(); }
      else m_Submitted = false;
      UpdateList();
      return true;
    }
    bool votesReady;
    unsigned revision;
    {
      std::lock_guard<std::mutex> lock(m_Votes->mutex);
      if (!m_Votes->ready && std::chrono::steady_clock::now() - m_Started > std::chrono::seconds(15))
      {
        m_Votes->info.note = "Poll request timed out. Select Refresh results to retry.";
        m_Votes->info.canVote = false;
        m_Votes->ready = true;
        ++m_Votes->generation;
        ++m_Votes->revision;
      }
      else if (m_Votes->ready && m_Votes->info.votersLoading &&
          std::chrono::steady_clock::now() - m_Started > std::chrono::seconds(15))
      {
        m_Votes->info.votersLoading = false;
        m_Votes->info.note = "Voter names timed out; list may be partial. Refresh results to retry.";
        ++m_Votes->generation;
        ++m_Votes->revision;
      }
      votesReady = m_Votes->ready;
      revision = m_Votes->revision;
    }
    if (votesReady && (!m_VotesDisplayed || revision != m_VotesRevision))
    {
      if (m_AwaitingRefresh)
      {
        m_Submitted = false;
        m_AwaitingRefresh = false;
        std::lock_guard<std::mutex> lock(m_State->mutex);
        m_State->result = "Vote saved.";
      }
      UpdateList(); m_VotesDisplayed = true; m_VotesRevision = revision; return true;
    }
    return false;
  }
  void UpdateList() override
  {
    const int previousAction = m_Index >= 0 && m_Index < static_cast<int>(m_Visible.size()) ? m_Visible[m_Index] : -1;
    const std::string previousRow = m_Index >= 0 && m_Index < static_cast<int>(m_RowKeys.size()) ? m_RowKeys[m_Index] : "";
    m_Items.clear();
    m_Visible.clear();
    m_RowKeys.clear();
    auto add = [&](const std::string& line, int option, const std::string& rowKey = "")
    {
      const bool choice = option >= 0;
      const auto content = choice ? line.substr(4) : line;
      const auto key = rowKey.empty() ? "action:" + std::to_string(option) : rowKey;
      const auto wrapped = StrUtil::WordWrap(StrUtil::ToWString(content),
        static_cast<unsigned>(std::max(1, m_W - (choice ? 4 : 0))), false, false, false, 0);
      for (size_t i = 0; i < wrapped.size(); ++i)
      {
        const auto prefix = choice ? StrUtil::ToWString(i == 0 ? line.substr(0, 4) : "    ") : L"";
        m_Items.push_back(StrUtil::TrimPadWString(prefix + wrapped[i], m_W));
        m_Visible.push_back(option);
        m_RowKeys.push_back(key + ":" + std::to_string(i));
      }
    };
    add(m_Question, -1, "question");
    std::string result;
    { std::lock_guard<std::mutex> lock(m_State->mutex); result = m_State->result; }
    if (!result.empty()) add(result, -1, "result");
    else if (m_Submitted) add("Submitting vote...", -1, "help");
    else add("Enter toggles a choice; select Submit to save your vote.", -1, "help");
    PollVoteInfo voteInfo;
    bool votesReady;
    {
      std::lock_guard<std::mutex> lock(m_Votes->mutex);
      voteInfo = m_Votes->info;
      votesReady = m_Votes->ready;
    }
    if (votesReady && voteInfo.selectionKnown)
    {
      if (!voteInfo.optionTexts.empty()) m_Options = voteInfo.optionTexts;
      if (voteInfo.maxChoices > 0) m_MaxChoices = voteInfo.maxChoices;
      if (!m_SelectionTouched)
      {
        m_Selected.clear();
        for (int option : voteInfo.selectedOptionIndexes)
          if (option >= 0 && option < static_cast<int>(m_Options.size())) m_Selected.insert(option);
      }
    }
    if (!voteInfo.summary.empty()) add(voteInfo.summary, -1, "summary");
    const auto filter = StrUtil::ToLower(StrUtil::ToString(m_FilterStr));
    for (size_t i = 0; i < m_Options.size(); ++i)
    {
      const bool matchesOption = filter.empty() || StrUtil::ToLower(m_Options[i]).find(filter) != std::string::npos;
      std::vector<std::pair<std::string, std::string>> names;
      if (votesReady)
        for (const auto& vote : voteInfo.votes)
          if (vote.optionIndex == static_cast<int>(i))
          {
            auto name = vote.userId == s_ReactionsSelfId ? "You" :
              m_Model->GetContactListName(m_Profile, vote.userId, true, true);
            if (name.empty()) name = vote.userId;
            if (matchesOption || StrUtil::ToLower(name).find(filter) != std::string::npos ||
                StrUtil::ToLower(vote.userId).find(filter) != std::string::npos)
              names.emplace_back(vote.userId, std::move(name));
          }
      if (matchesOption || !names.empty())
      {
        add((m_Selected.count(static_cast<int>(i)) ? "[x] " : "[ ] ") +
          m_Options[i], static_cast<int>(i));
        for (const auto& name : names)
          add("    " + name.second, -1, "voter:" + std::to_string(i) + ":" + name.first);
      }
    }
    if (votesReady && !m_Submitted && !m_Unconfirmed && voteInfo.canVote)
    {
      add("Submit vote (" + std::to_string(m_Selected.size()) + "/" + std::to_string(m_MaxChoices) + ")", -2);
      if (!m_Selected.empty()) add(voteInfo.canRetract ?
        "Clear selection (Submit to retract)" : "Clear selection", -3);
    }
    if (votesReady && !m_Submitted) add("Refresh results and voters", -4);
    if (!votesReady) add("Loading voters...", -1, "note");
    else
    {
      if (voteInfo.votersLoading) add("Loading more voter names (" +
        std::to_string(voteInfo.votes.size()) + " vote records received)...", -1, "loading");
      if (!voteInfo.note.empty() && (!voteInfo.votersLoading || voteInfo.note != "Loading voter names..."))
        add(voteInfo.note, -1, "note");
    }
    const auto row = std::find(m_RowKeys.begin(), m_RowKeys.end(), previousRow);
    if (!previousRow.empty() && row != m_RowKeys.end()) m_Index = static_cast<int>(row - m_RowKeys.begin());
    else if (previousAction != -1)
    {
      const auto action = std::find(m_Visible.begin(), m_Visible.end(), previousAction);
      m_Index = action == m_Visible.end() ? 0 : static_cast<int>(action - m_Visible.begin());
    }
    else m_Index = 0; // A disappeared name/status must never turn into a selectable choice.
  }
};
