// Synthetic poll metadata and the production dialog, never a live vote.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <ncurses.h>
#include <unistd.h>

#include "appconfig.h"
#include "fileutil.h"
#include "messagecache.h"
#include "tgpoll_util.h"
#include "uicolorconfig.h"
#include "uiconfig.h"
#include "uicontroller.h"
#include "uikeyconfig.h"
#include "uipolldialog.h"

UiController::UiController() {}
UiController::~UiController() {}
void UiController::Init() {}
void UiController::Cleanup() {}
wint_t UiController::GetKey(int) { return UiKeyConfig::GetKey("cancel"); }

struct PollProtocol : Protocol
{
  PollVoteInfo info;
  int votes = 0, reads = 0;
  bool fail = false;
  bool deferVote = false;
  std::vector<int> lastSelection;
  std::function<void(PollVoteInfo)> voterPage;
  std::function<void(bool, std::string)> pendingVote;
  std::string GetProfileId() const override { return "poll-test"; }
  std::string GetProfileDisplayName() const override { return "Offline"; }
  bool HasFeature(ProtocolFeature) const override { return false; }
  bool IsGroupChat(const std::string&) const override { return false; }
  std::string GetSelfId() const override { return "self"; }
  bool SetupProfile(const std::string&, std::string&) override { return false; }
  bool LoadProfile(const std::string&, const std::string&) override { return false; }
  bool CloseProfile() override { return true; }
  bool Login() override { return false; }
  bool Logout() override { return true; }
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>&) override {}
  void SendRequest(std::shared_ptr<RequestMessage> request) override
  {
    if (request->GetMessageType() == GetPollVotesRequestType)
    {
      ++reads;
      voterPage = std::static_pointer_cast<GetPollVotesRequest>(request)->onResult;
      voterPage(info);
    }
    else if (request->GetMessageType() == VotePollRequestType)
    {
      ++votes;
      auto vote = std::static_pointer_cast<VotePollRequest>(request);
      lastSelection = vote->optionIndexes;
      if (deferVote) { pendingVote = vote->onResult; return; }
      if (!fail) info.selectedOptionIndexes = lastSelection;
      vote->onResult(!fail, fail ? "Synthetic service refusal" : "Saved");
    }
    else assert(false && "Unexpected service request");
  }
};

static void Select(UiPollDialog& dialog, int option)
{
  auto found = std::find(dialog.m_Visible.begin(), dialog.m_Visible.end(), option);
  assert(found != dialog.m_Visible.end());
  dialog.m_Index = static_cast<int>(found - dialog.m_Visible.begin());
  dialog.KeyHandler(UiKeyConfig::GetKey("ok"));
}

int main()
{
  auto poll = td::td_api::make_object<td::td_api::poll>();
  poll->question_ = td::td_api::make_object<td::td_api::formattedText>();
  poll->question_->text_ = "Drink?";
  poll->allows_revoting_ = true;
  poll->can_see_results_ = true;
  poll->total_voter_count_ = 5;
  for (const auto& label : {"Tea", "Coffee"})
  {
    auto option = td::td_api::make_object<td::td_api::pollOption>();
    option->text_ = td::td_api::make_object<td::td_api::formattedText>();
    option->text_->text_ = label;
    option->voter_count_ = 2;
    option->vote_percentage_ = 40;
    poll->options_.push_back(std::move(option));
  }
  poll->options_[1]->is_chosen_ = true;
  auto quiz = td::td_api::make_object<td::td_api::pollTypeQuiz>();
  quiz->correct_option_ids_ = {0};
  quiz->explanation_ = td::td_api::make_object<td::td_api::formattedText>();
  quiz->explanation_->text_ = "Explanation supplied by Telegram";
  poll->type_ = std::move(quiz);
  const auto text = TgPollText(*poll);
  assert(text.find("2 votes, 40%") != std::string::npos);
  assert(text.find("5 total voters") != std::string::npos);
  assert(text.find("[Correct]") != std::string::npos);
  assert(text.find("Explanation supplied by Telegram") != std::string::npos);
  assert(text.find("[Your vote]") != std::string::npos);
  assert(TgPollAnswerAllowed(*poll, {}));
  assert(!TgPollAnswerAllowed(*poll, {0, 1}));
  assert(!TgPollAnswerAllowed(*poll, {-1}));
  poll->allows_revoting_ = false;
  assert(!TgPollAnswerAllowed(*poll, {}) && !TgPollAnswerAllowed(*poll, {0}));
  poll->allows_revoting_ = true;
  poll->is_closed_ = true;
  assert(!TgPollAnswerAllowed(*poll, {0}));
  poll->is_closed_ = false;

  char temp[] = "/tmp/nchat-poll-test.XXXXXX";
  const auto root = mkdtemp(temp);
  assert(root);
  FileUtil::SetApplicationDir(root);
  FileUtil::InitTempDir();
  { std::ofstream config(std::string(root) + "/ui.conf"); config << "thumbnail_command=/usr/bin/true\n"; }
  AppConfig::Init(); UiConfig::Init(); MessageCache::Init();
  FILE* output = tmpfile(); FILE* source = tmpfile();
  assert(output && source);
  SCREEN* terminal = newterm("xterm-256color", output, source);
  assert(terminal); set_term(terminal); assert(resizeterm(45, 140) != ERR);
  UiColorConfig::Init(); UiKeyConfig::Init(false);
  {
    UiModel model;
    auto protocol = std::make_shared<PollProtocol>();
    model.AddProtocol(protocol);
    model.Init();
    protocol->info = TgPollInfo(*poll);
    auto request = std::make_shared<VotePollRequest>();
    request->chatId = "chat"; request->msgId = "poll";
    UiDialogParams params(&model, "Poll", 0.75, 0.5);
    UiPollDialog dialog(params, protocol, request, "poll-test", text);
    dialog.OnTimer();
    assert(dialog.m_Selected == std::set<int>{1});
    const auto oldVoterPage = protocol->voterPage;
    Select(dialog, 0);
    assert(protocol->votes == 0 && dialog.m_Selected == std::set<int>{0});
    auto paged = protocol->info;
    paged.optionTexts[1] = "Coffee (3 votes, 60%)";
    protocol->voterPage(paged); dialog.OnTimer();
    assert(dialog.m_Options[1] == "Coffee (3 votes, 60%)" && dialog.m_Selected == std::set<int>{0});
    Select(dialog, -2);
    assert(protocol->votes == 1 && protocol->lastSelection == std::vector<int>{0});
    dialog.OnTimer(); dialog.OnTimer();
    assert(protocol->reads == 2 && !dialog.m_Submitted);
    oldVoterPage(paged); dialog.OnTimer();
    assert(dialog.m_Selected == std::set<int>{0});
    Select(dialog, -3);
    assert(protocol->votes == 1 && dialog.m_Selected.empty());
    Select(dialog, -2);
    assert(protocol->votes == 2 && protocol->lastSelection.empty());
    dialog.OnTimer(); dialog.OnTimer();
    protocol->fail = true;
    Select(dialog, 1); Select(dialog, -2); dialog.OnTimer();
    assert(!dialog.m_Submitted && protocol->votes == 3);
    protocol->fail = false;
    protocol->info.canVote = false;
    Select(dialog, -4); dialog.OnTimer();
    Select(dialog, 0);
    assert(protocol->votes == 3 && dialog.m_Selected.empty());
  }
  {
    UiModel model;
    auto protocol = std::make_shared<PollProtocol>();
    model.AddProtocol(protocol); model.Init();
    poll->allows_multiple_answers_ = true;
    protocol->info = TgPollInfo(*poll);
    auto request = std::make_shared<VotePollRequest>();
    UiDialogParams params(&model, "Poll", 0.75, 0.5);
    UiPollDialog dialog(params, protocol, request, "poll-test", text);
    dialog.OnTimer(); Select(dialog, 0);
    assert(protocol->votes == 0 && dialog.m_Selected.size() == 2);
    Select(dialog, -2);
    assert(protocol->votes == 1 && protocol->lastSelection.size() == 2);
  }
  {
    UiModel model;
    auto protocol = std::make_shared<PollProtocol>();
    model.AddProtocol(protocol); model.Init();
    poll->allows_multiple_answers_ = false;
    protocol->info = TgPollInfo(*poll);
    protocol->info.votersLoading = true;
    protocol->info.note = "Loading voter names...";
    protocol->deferVote = true;
    auto request = std::make_shared<VotePollRequest>();
    UiDialogParams params(&model, "Poll", 0.75, 0.5);
    UiPollDialog dialog(params, protocol, request, "poll-test", text);
    dialog.OnTimer();
    dialog.m_Started -= std::chrono::seconds(16); // Fast clock advance, no real wait.
    dialog.OnTimer();
    assert(!dialog.m_Votes->info.votersLoading && dialog.m_Votes->info.canVote);
    assert(dialog.m_Votes->info.note.find("timed out") != std::string::npos);
    Select(dialog, 0); Select(dialog, -2);
    assert(protocol->votes == 1 && dialog.m_Submitted);
    const auto lateAcknowledgement = protocol->pendingVote;
    dialog.m_SubmitStarted -= std::chrono::seconds(31);
    dialog.OnTimer();
    assert(!dialog.m_Submitted && dialog.m_Unconfirmed && protocol->votes == 1);
    assert(std::find(dialog.m_Visible.begin(), dialog.m_Visible.end(), -2) == dialog.m_Visible.end());
    lateAcknowledgement(true, "Late success"); dialog.OnTimer();
    assert(dialog.m_Unconfirmed && !dialog.m_State->done && protocol->votes == 1);
    protocol->info.votersLoading = false;
    protocol->info.note.clear();
    Select(dialog, -4); dialog.OnTimer();
    assert(!dialog.m_Unconfirmed && dialog.m_Selected == std::set<int>{1});
    Select(dialog, 0); Select(dialog, -2);
    assert(protocol->votes == 2 && dialog.m_Submitted);
    lateAcknowledgement(true, "Very late success"); dialog.OnTimer();
    assert(dialog.m_Submitted && !dialog.m_State->done);
    // Back always exits, including while waiting for a service acknowledgement.
    dialog.KeyHandler(UiKeyConfig::GetKey("cancel"));
    assert(!dialog.m_Running && protocol->votes == 2);
  }
  endwin(); delscreen(terminal); fclose(output); fclose(source);
  MessageCache::Cleanup(); UiKeyConfig::Cleanup(); UiColorConfig::Cleanup(); UiConfig::Cleanup(); AppConfig::Cleanup();
  FileUtil::RmDir(root);
  std::cout << "Poll metadata, explicit submission, own-choice, retract, refresh and retry tests passed\n";
}
