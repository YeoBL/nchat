#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <td/telegram/td_api.h>
#include "protocol.h"

inline PollVoteInfo TgPollInfo(const td::td_api::poll& poll)
{
  PollVoteInfo info;
  info.selectionKnown = true;
  info.maxChoices = poll.allows_multiple_answers_ ? static_cast<int>(poll.options_.size()) : 1;
  info.canRetract = !poll.is_closed_ && poll.allows_revoting_;
  for (size_t i = 0; i < poll.options_.size(); ++i)
  {
    const auto& option = poll.options_[i];
    std::string text = option && option->text_ ? option->text_->text_ : "";
    if (option && option->is_chosen_) info.selectedOptionIndexes.push_back(static_cast<int>(i));
    if (option && poll.can_see_results_)
      text += " (" + std::to_string(option->voter_count_) + " votes, " +
        std::to_string(option->vote_percentage_) + "%)";
    if (poll.type_ && poll.type_->get_id() == td::td_api::pollTypeQuiz::ID)
    {
      const auto& quiz = static_cast<const td::td_api::pollTypeQuiz&>(*poll.type_);
      if (std::find(quiz.correct_option_ids_.begin(), quiz.correct_option_ids_.end(),
          static_cast<int>(i)) != quiz.correct_option_ids_.end()) text += " [Correct]";
    }
    info.optionTexts.push_back(std::move(text));
  }
  info.canVote = !poll.is_closed_ && !poll.vote_restriction_reason_ &&
    (poll.allows_revoting_ || info.selectedOptionIndexes.empty());
  if (poll.can_see_results_)
    info.summary = std::to_string(poll.total_voter_count_) + " total voters";
  if (poll.type_ && poll.type_->get_id() == td::td_api::pollTypeQuiz::ID)
  {
    info.summary += (info.summary.empty() ? "" : "\n") + std::string("[Quiz]");
    const auto& quiz = static_cast<const td::td_api::pollTypeQuiz&>(*poll.type_);
    if (quiz.explanation_ && !quiz.explanation_->text_.empty())
      info.summary += "\n" + quiz.explanation_->text_;
  }
  if (poll.is_closed_) info.summary += "\n[Closed]";
  else if (!info.canVote) info.summary += "\n[Voting unavailable]";
  return info;
}

inline bool TgPollAnswerAllowed(const td::td_api::poll& poll, const std::vector<int>& choices)
{
  const auto info = TgPollInfo(poll);
  if (!info.canVote || (choices.empty() && !info.canRetract) ||
      (!poll.allows_multiple_answers_ && choices.size() > 1)) return false;
  std::vector<int> seen;
  for (int index : choices)
  {
    if (index < 0 || index >= static_cast<int>(poll.options_.size()) ||
        std::find(seen.begin(), seen.end(), index) != seen.end()) return false;
    seen.push_back(index);
  }
  return true;
}

inline std::string TgPollText(const td::td_api::poll& poll)
{
  const auto info = TgPollInfo(poll);
  std::string text = "[Poll] " + (poll.question_ ? poll.question_->text_ : "");
  for (size_t i = 0; i < info.optionTexts.size(); ++i)
  {
    text += "\n• " + info.optionTexts[i];
    if (std::find(info.selectedOptionIndexes.begin(), info.selectedOptionIndexes.end(),
        static_cast<int>(i)) != info.selectedOptionIndexes.end()) text += " [Your vote]";
  }
  if (poll.allows_multiple_answers_) text += "\n[Choose multiple]";
  if (!info.summary.empty()) text += "\n" + info.summary;
  return text;
}
