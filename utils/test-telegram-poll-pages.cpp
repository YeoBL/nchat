// Production Telegram pagination callbacks, with synthetic replies and no
// ClientManager/login/network or real vote. One deadline check waits 15.1 s.
#include "../lib/tgchat/src/tgchat.cpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace td::td_api;

namespace
{
object_ptr<td::td_api::message> PollMessage(bool namedVoters = true)
{
  auto message = make_object<td::td_api::message>();
  message->id_ = 10;
  message->chat_id_ = 1001;
  auto content = make_object<messagePoll>();
  content->poll_ = make_object<poll>();
  auto& value = *content->poll_;
  value.question_ = make_object<formattedText>();
  value.question_->text_ = "Synthetic poll";
  value.can_get_voters_ = namedVoters;
  value.can_see_results_ = true;
  value.allows_revoting_ = true;
  value.total_voter_count_ = 4;
  for (const auto& label : {"Synthetic first option", "Synthetic second option"})
  {
    auto option = make_object<pollOption>();
    option->text_ = make_object<formattedText>();
    option->text_->text_ = label;
    option->voter_count_ = 2;
    option->vote_percentage_ = 50;
    value.options_.push_back(std::move(option));
  }
  value.options_[0]->is_chosen_ = true;
  message->content_ = std::move(content);
  return message;
}

object_ptr<pollVoters> Page(int total, std::initializer_list<int64_t> users)
{
  auto page = make_object<pollVoters>();
  page->total_count_ = total;
  for (int64_t id : users)
    page->voters_.push_back(make_object<pollVoter>(make_object<messageSenderUser>(id), 1700000000));
  return page;
}
}

// This friend already exists for the offline mute-handler regression.
struct TelegramMuteHandlerTest
{
  struct Fixture
  {
    TgChat::Impl impl;
    std::vector<PollVoteInfo> snapshots;
    std::shared_ptr<bool> canceled = std::make_shared<bool>(false);
    std::weak_ptr<GetPollVotesRequest> request;

    void Start()
    {
      assert(!impl.m_ClientManager);
      auto query = std::make_shared<GetPollVotesRequest>();
      query->chatId = StrUtil::NumToHex(int64_t(1001));
      query->msgId = StrUtil::NumToHex(int64_t(10));
      query->isCancelled = [flag = canceled] { return *flag; };
      query->onResult = [this](PollVoteInfo info) { snapshots.push_back(std::move(info)); };
      request = query;
      impl.PerformRequest(query);
      assert(impl.m_Handlers.size() == 1);
    }

    void Reply(object_ptr<Object> object)
    {
      assert(!impl.m_ClientManager && impl.m_Handlers.size() == 1);
      impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(object)});
    }

    void CheckMetadata() const
    {
      for (const auto& snapshot : snapshots)
      {
        assert(snapshot.selectionKnown && snapshot.canVote && snapshot.canRetract);
        assert(snapshot.maxChoices == 1 && snapshot.selectedOptionIndexes == std::vector<int>{0});
        assert(snapshot.optionTexts.size() == 2);
        assert(snapshot.optionTexts[0].find("Synthetic first option") != std::string::npos);
        assert(snapshot.summary == "4 total voters");
      }
    }

    void CheckReleased() const
    {
      assert(!impl.m_ClientManager && impl.m_Handlers.empty());
      assert(request.expired()); // A recursive fetch callback must not own itself.
    }
  };

  static void Run()
  {
    {
      Fixture test;
      test.Start();
      assert(test.snapshots.empty());
      test.Reply(PollMessage());
      assert(test.snapshots.size() == 1 && test.snapshots.back().votersLoading);
      assert(test.snapshots.back().votes.empty() && test.impl.m_Handlers.size() == 1);
      // Raw page size is two even though the second row repeats the same
      // identity. The subsequent page must continue at offset 2, not 1.
      test.Reply(Page(3, {11, 11}));
      assert(test.snapshots.size() >= 2 && test.snapshots.back().votersLoading);
      assert(test.snapshots.back().votes.size() == 1);
      assert(test.snapshots.back().votes.front().userId == StrUtil::NumToHex(int64_t(11)));
      assert(test.impl.m_Handlers.size() == 1);
      test.Reply(Page(3, {12}));
      assert(test.snapshots.back().votes.size() == 2 && test.snapshots.back().votersLoading);
      assert(test.impl.m_Handlers.size() == 1);
      test.Reply(Page(1, {13}));
      assert(!test.snapshots.back().votersLoading && test.snapshots.back().votes.size() == 3);
      assert(test.snapshots.back().votes.back().optionIndex == 1);
      assert(test.snapshots.back().votes.back().userId == StrUtil::NumToHex(int64_t(13)));
      size_t finals = 0;
      for (const auto& snapshot : test.snapshots) if (!snapshot.votersLoading) ++finals;
      assert(finals == 1);
      test.CheckMetadata();
      test.CheckReleased();
    }
    std::cout << "PASS: immediate poll metadata, cumulative short pages, identity deduplication, raw offsets and one complete result\n";

    {
      Fixture test;
      test.Start(); test.Reply(PollMessage());
      test.Reply(Page(1, {21}));
      assert(test.snapshots.back().votersLoading && test.snapshots.back().votes.size() == 1);
      test.Reply(make_object<error>(503, "Synthetic inaccessible second option"));
      assert(!test.snapshots.back().votersLoading && test.snapshots.back().votes.size() == 1);
      assert(!test.snapshots.back().note.empty());
      test.CheckMetadata(); test.CheckReleased();
    }
    {
      Fixture test;
      test.Start(); test.Reply(PollMessage());
      test.Reply(Page(5, {})); // Empty short page must stop this option, not loop.
      test.Reply(Page(1, {22}));
      assert(!test.snapshots.back().votersLoading && test.snapshots.back().votes.size() == 1);
      assert(test.snapshots.back().votes.front().optionIndex == 1 && !test.snapshots.back().note.empty());
      test.CheckMetadata(); test.CheckReleased();
    }
    {
      Fixture test;
      test.Start(); test.Reply(PollMessage(false));
      assert(test.snapshots.size() == 1 && !test.snapshots.front().votersLoading &&
        test.snapshots.front().votes.empty() && !test.snapshots.front().note.empty());
      test.CheckMetadata(); test.CheckReleased();
    }
    std::cout << "PASS: partial-error/empty-page recovery and undisclosed-voter results preserve poll metadata\n";

    {
      Fixture test;
      test.Start();
      *test.canceled = true;
      const auto lastQuery = test.impl.m_CurrentQueryId;
      test.Reply(PollMessage());
      assert(test.snapshots.empty() && test.impl.m_CurrentQueryId == lastQuery);
      test.CheckReleased();
    }
    {
      Fixture test;
      test.Start(); test.Reply(PollMessage()); test.Reply(Page(3, {31}));
      const auto snapshots = test.snapshots.size();
      const auto lastQuery = test.impl.m_CurrentQueryId;
      *test.canceled = true;
      test.Reply(Page(3, {32}));
      assert(test.snapshots.size() == snapshots && test.impl.m_CurrentQueryId == lastQuery);
      test.CheckReleased();
    }
    std::cout << "PASS: canceled metadata/page replies schedule nothing, publish nothing and release recursive request ownership\n";

    {
      Fixture test;
      test.Start(); test.Reply(PollMessage()); test.Reply(Page(3, {41}));
      const auto lastQuery = test.impl.m_CurrentQueryId;
      std::this_thread::sleep_for(std::chrono::milliseconds(15100));
      test.Reply(Page(3, {42}));
      assert(test.impl.m_CurrentQueryId == lastQuery);
      assert(!test.snapshots.back().votersLoading && test.snapshots.back().votes.size() == 2);
      assert(!test.snapshots.back().note.empty());
      test.CheckMetadata(); test.CheckReleased();
    }
    std::cout << "PASS: real steady-clock voter deadline stops subsequent paging and retains the received partial list\n";
  }
};

int main(int argc, char** argv)
{
  assert(argc == 2);
  FileUtil::SetApplicationDir(argv[1]);
  AppConfig::Init();
  AppConfig::SetBool("cache_enabled", false);
  MessageCache::Init();
  TelegramMuteHandlerTest::Run();
  MessageCache::Cleanup();
  AppConfig::Cleanup();
}
