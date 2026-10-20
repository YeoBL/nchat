// Link the actual application objects, replacing only terminal drawing.
// No providers are registered: these tests cannot send messages/read receipts.
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <cassert>
#include <iostream>
#include "protocol.h"
#include "apputil.h"
#include "owned_mutex.h"
#define private public
#include "../src/uimodel.h"
#undef private
#include "../src/uiview.h"
#include "../src/uiconfig.h"
#include "fileutil.h"
#include "../src/terminalbadge.h"
#include "test-message-info-dialog.h"
#include "sysutil.h"

// Do not run the real thumbnail helper against the user's terminal in UI tests.
bool SysUtil::RunCommand(const std::string& command, std::string*)
{
  assert(command == "nchat-thumbnails");
  return true;
}

static int bells = 0, clears = 0;
static bool badgeVisible = false, badgeWriteFails = false;
static bool liveDock = false;
UiView::UiView(UiModel*) {}
UiView::~UiView() {}
void UiView::TerminalBell() { ++bells; }
void UiView::ClearTerminalBadge() { ++clears; badgeVisible = false; }
bool UiView::SyncTerminalBadge(bool active)
{
  if (badgeWriteFails) return false;
  if (liveDock && !SetKittyBadge(active)) return false;
  badgeVisible = active;
  if (active) ++bells; else ++clears;
  return true;
}
void UiView::Init() {}
void UiView::Draw() {}
#define DIRTY(name) void UiView::name(bool) {}
DIRTY(SetListDirty) DIRTY(SetStatusDirty) DIRTY(SetHistoryDirty)
DIRTY(SetHelpDirty) DIRTY(SetEntryDirty)
#define TOGGLE(name) void UiView::Set##name(bool) {} bool UiView::Get##name() { return true; }
TOGGLE(EmojiEnabled) TOGGLE(TopEnabled) TOGGLE(HelpEnabled) TOGGLE(ListEnabled)
int UiView::GetHistoryShowCount() { return 20; }
int UiView::GetHistoryLines() { return 30; }
int UiView::GetEntryWidth() { return 80; }
int UiView::GetScreenWidth() { return 100; }
int UiView::GetScreenHeight() { return 40; }
void UiView::DecreaseListWidth() {}
void UiView::IncreaseListWidth() {}

static void incoming(UiModel::Impl& model, const std::string& id,
                     const std::string& text = "hello", bool cached = false,
                     const std::string& profile = "tg", const std::string& chat = "chat", bool hasMention = false)
{
  model.m_ConnectTime[profile] = 1;
  model.m_ChatInfos[profile][chat].id = chat;
  auto event = std::make_shared<NewMessagesNotify>(profile);
  event->success = true; event->chatId = chat; event->cached = cached;
  ChatMessage message;
  static int64_t timestamp = 100;
  message.id = id; message.text = text; message.timeSent = ++timestamp;
  message.isOutgoing = false;
  message.hasMention = hasMention;
  event->chatMessages.push_back(message);
  model.MessageHandler(event);
}

static void read(UiModel::Impl& model, const std::string& id, const std::string& through = "",
                 const std::string& profile = "tg", const std::string& chat = "chat")
{
  auto event = std::make_shared<NewMessageStatusNotify>(profile);
  event->chatId = chat; event->msgId = id; event->readThroughId = through; event->isRead = true;
  model.MessageHandler(event);
}

int main(int argc, char** argv)
{
  assert(argc == 2 || argc == 3);
  FileUtil::SetApplicationDir(argv[1]);
  UiConfig::Init();
  UiConfig::SetBool("terminal_bell_active", true);
  UiConfig::SetBool("terminal_bell_inactive", true);
  UiConfig::SetBool("desktop_notify_active_current", false);
  UiConfig::SetBool("desktop_notify_active_noncurrent", false);
  UiConfig::SetBool("desktop_notify_inactive", false);
  UiConfig::SetBool("online_status_dynamic", false);
  UiConfig::SetBool("typing_status_share", false);
  UiConfig::SetNum("auto_select_chat_timeout_sec", 0);
  {
    setenv("NCHAT_NO_AUTO_SELECT", "1", 1);
    UiModel::Impl model(nullptr);
    unsetenv("NCHAT_NO_AUTO_SELECT");
    model.m_ChatVec.emplace_back("tg", "chat");
    assert(!model.SetCurrentChatIndexIfNotSet());
    assert(model.m_CurrentChatIndex == -1);
    model.AnyUserKeyInput();
    assert(model.m_CurrentChatIndex == 0 && !model.m_DeferInitialChat);
    UiModel::Impl normal(nullptr);
    assert(!normal.m_DeferInitialChat);
  }
  {
    UiModel::Impl model(nullptr);
    incoming(model, "313030"); model.Process(); assert(bells == 1);
    read(model, "", "3939"); model.Process(); assert(clears == 0);
    read(model, "", "313030"); model.Process(); assert(clears == 1);
    incoming(model, "313030", "hello", true); model.Process(); assert(bells == 1);
    assert(model.m_Messages["tg"]["chat"]["313030"].isRead);
  }
  {
    UiModel::Impl model(nullptr);
    incoming(model, "1", "[Joined]"); model.Process(); assert(bells == 1);
    incoming(model, "2", "[Added someone]"); model.Process(); assert(bells == 1);
    incoming(model, "3", "[Left]"); model.Process(); assert(bells == 1);
    incoming(model, "4", "[Removed someone]"); model.Process(); assert(bells == 1);
  }
  {
    UiModel::Impl model(nullptr);
    incoming(model, "1"); model.Process(); assert(bells == 2);
    read(model, "outgoing"); model.Process(); assert(clears == 1);
    read(model, ""); model.Process(); assert(clears == 2);
  }
  std::cout << "production handlers: basic delivery, partial/whole reads, joins, stale replay passed\n";
  {
    UiModel::Impl model(nullptr);
    read(model, "", "313030");
    incoming(model, "313030", "already read before loading", true);
    model.Process(); assert(bells == 2);
  }
  {
    UiModel::Impl model(nullptr);
    read(model, "early-receipt");
    incoming(model, "early-receipt"); model.Process(); assert(bells == 2);
  }
  std::cout << "production handlers: receipt-before-delivery ordering passed\n";
  {
    UiModel::Impl model(nullptr);
    incoming(model, "1");
    incoming(model, "2"); model.Process(); assert(bells == 3);
    incoming(model, "wa1", "hello", false, "wa"); model.Process(); assert(bells == 4);
    read(model, "2"); model.Process(); assert(clears == 2);
    read(model, "1"); model.Process(); assert(clears == 2);
    read(model, "", "", "wa"); model.Process(); assert(clears == 3);
  }
  {
    UiModel::Impl model(nullptr);
    incoming(model, "read-before-tick");
    read(model, "read-before-tick"); model.Process(); assert(bells == 4);
    model.m_ChatInfos["tg"]["chat"].isMuted = true;
    incoming(model, "muted"); model.Process(); assert(bells == 4);
    model.m_ChatInfos["tg"]["chat"].isArchived = true;
    incoming(model, "archived"); model.Process(); assert(bells == 4);
  }
  {
    UiModel::Impl model(nullptr);
    incoming(model, "local"); model.Process(); assert(bells == 5);
    model.m_CurrentChatIndex = 0;
    model.MarkRead("tg", "chat", "local", true);
    model.Process(); assert(clears == 4);
  }
  std::cout << "production handlers: multiple chats/messages, mute/archive, queued and local reads passed\n";
  {
    UiModel::Impl model(nullptr);
    model.m_ConnectTime["tg"] = 1;
    model.m_ChatInfos["tg"]["chat"].id = "chat";
    auto event = std::make_shared<NewMessagesNotify>("tg");
    event->success = true; event->chatId = "chat";
    ChatMessage older; older.id = "batch1"; older.timeSent = 100; older.isOutgoing = false;
    ChatMessage newer = older; newer.id = "batch2"; newer.timeSent = 101;
    event->chatMessages = {older, newer};
    model.MessageHandler(event); model.Process(); assert(bells == 6);
    read(model, "batch1"); model.Process(); assert(clears == 4);
    read(model, "batch2"); model.Process(); assert(clears == 5);
  }
  std::cout << "production handlers: batched incoming messages passed\n";
  {
    UiModel::Impl model(nullptr);
    incoming(model, "delete-me"); model.Process(); assert(bells == 7);
    auto event = std::make_shared<DeleteMessageNotify>("tg");
    event->success = true; event->chatId = "chat"; event->msgId = "delete-me";
    model.MessageHandler(event); model.Process(); assert(clears == 6);
  }
  {
    UiModel::Impl model(nullptr);
    incoming(model, "evicted"); model.Process(); assert(bells == 8);
    model.m_Messages.clear(); model.m_MessageVec.clear();
    read(model, "evicted"); model.Process(); assert(clears == 7);
  }
  {
    UiModel::Impl model(nullptr);
    read(model, "", "313031");
    read(model, "", "3939"); // Out-of-order older marker must not undo the newer one.
    incoming(model, "313030"); model.Process(); assert(bells == 8);
    incoming(model, "313032"); model.Process(); assert(bells == 9);
    read(model, "", "313032"); model.Process(); assert(clears == 8);
  }
  {
    UiModel::Impl model(nullptr);
    model.m_TerminalActive = false;
    incoming(model, "background"); model.Process(); assert(bells == 10);
    model.MarkRead("tg", "chat", "background", true); // Not actually viewed.
    model.Process(); assert(clears == 8);
    read(model, "background"); model.Process(); assert(clears == 9);
  }
  std::cout << "production handlers: deletion, evicted history, marker regression, inactive reads passed\n";
  {
    UiModel::Impl model(nullptr);
    incoming(model, "focus"); model.Process(); assert(badgeVisible);
    badgeVisible = false; // Simulate Kitty's documented clear-on-focus behavior.
    model.m_NextBadgeSyncMs = 0; model.Process(); assert(badgeVisible);
    badgeWriteFails = true;
    read(model, "focus"); model.Process(); assert(badgeVisible);
    assert(model.m_NextBadgeSyncMs > 0); // Failed clear must remain scheduled.
    badgeWriteFails = false;
    model.m_NextBadgeSyncMs = 0; model.Process(); assert(!badgeVisible);
    assert(model.m_NextBadgeSyncMs == -1); // Stop writing after a successful clear.
    badgeWriteFails = true;
    incoming(model, "retry-set"); model.Process(); assert(!badgeVisible);
    badgeWriteFails = false;
    model.m_NextBadgeSyncMs = 0; model.Process(); assert(badgeVisible);
    read(model, "retry-set"); model.Process(); assert(!badgeVisible);
  }
  std::cout << "production badge scheduling: external clear, failed set/clear retries passed\n";
  {
    UiModel::Impl model(nullptr);
    badgeVisible = true;
    model.Init(); model.Process(); assert(!badgeVisible);
    incoming(model, "shutdown"); model.Process(); assert(badgeVisible);
    model.Cleanup(); assert(!badgeVisible);
  }
  std::cout << "production badge lifecycle: startup and clean shutdown passed\n";
  {
    UiModel::Impl model(nullptr);
    const auto before = clears;
    incoming(model, "missing-individual-receipt", "hello", false, "wa");
    incoming(model, "newer-read-message", "hello", false, "wa");
    model.Process();
    read(model, "newer-read-message", "", "wa");
    model.Process();
    assert(clears == before + 1); // Read progress must retire older orphaned alerts.
    incoming(model, "missing-individual-receipt", "cached", true, "wa");
    assert(model.m_Notifications.PendingChats() == 0);
  }
  {
    // Reproduce the observed gap: an old unacknowledged WhatsApp message,
    // followed by loading a newer message that is already marked read.
    UiModel::Impl model(nullptr);
    auto event = std::make_shared<NewMessagesNotify>("wa");
    model.m_ConnectTime["wa"] = 1;
    model.m_ChatInfos["wa"]["chat"].id = "chat";
    event->success = true; event->chatId = "chat";
    ChatMessage old; old.id = "old"; old.isOutgoing = false; old.timeSent = 1789042903149;
    event->chatMessages = {old};
    model.MessageHandler(event); model.Process();
    const auto before = clears;
    ChatMessage latest = old; latest.id = "latest"; latest.isRead = true; latest.timeSent = 1789046374254;
    event->cached = true; event->chatMessages = {latest};
    model.MessageHandler(event); model.Process();
    assert(clears == before + 1);
    event->chatMessages = {old}; // Stale cache cannot resurrect the old alert.
    model.MessageHandler(event); model.Process();
    assert(model.m_Notifications.PendingChats() == 0);
    assert(!model.m_Messages["wa"]["chat"]["old"].isRead); // No fabricated per-message receipt.
  }
  std::cout << "production read-progress regressions: missing receipt and later cached read passed\n";
  TestMessageInfoDialog();
  {
    UiModel::Impl model(nullptr);
    const std::string profile = "Telegram_membership";
    model.m_CurrentChatIndex = 0;
    model.m_CurrentChat = {profile, "other"};
    auto chats = std::make_shared<NewChatsNotify>(profile);
    chats->success = true;
    ChatInfo left, other;
    left.id = "left"; other.id = "other";
    chats->chatInfos = {left, other};
    model.MessageHandler(chats);
    incoming(model, "cached", "keep this history", true, profile, "left");
    auto membership = std::make_shared<UpdateMembershipNotify>(profile);
    membership->chatId = "left"; membership->isMember = false;
    model.MessageHandler(membership);
    assert(model.m_ChatVec.size() == 1 && model.m_ChatVec.front().second == "other");
    assert(model.m_CurrentChat.second == "other");
    assert(model.m_Messages[profile]["left"].count("cached"));
    assert(!model.m_Messages[profile]["left"]["cached"].isRead);
    model.MessageHandler(chats); // Stale metadata must not resurrect the channel.
    incoming(model, "late", "late cached post", true, profile, "left");
    assert(model.m_ChatVec.size() == 1);
    assert(model.m_Notifications.PendingChats() == 0);
    membership->isMember = true;
    model.MessageHandler(membership);
    assert(model.m_ChatVec.size() == 2); // Rejoin restores the list, not a replayed alert.
    model.m_CurrentChat = {profile, "left"};
    membership->isMember = false;
    model.MessageHandler(membership);
    assert(model.m_CurrentChat.first.empty()); // No automatic read of a different chat.
    UiModel::Impl startup(nullptr);
    startup.m_CurrentChatIndex = 0;
    startup.MessageHandler(membership); // Cached membership precedes chat enumeration.
    startup.MessageHandler(chats);
    assert(startup.m_ChatVec.size() == 1 && startup.m_ChatVec.front().second == "other");
    membership->chatId = "other";
    startup.m_CurrentChat = {profile, "other"};
    startup.MessageHandler(membership);
    assert(startup.m_ChatVec.empty() && startup.m_CurrentChat.first.empty());
    std::cout << "PASS: production nchat list excludes left chats on startup/live refresh, preserves history/read flags, suppresses late alerts, restores rejoined chats, safely handles last/selected chat\n";
  }
  {
    UiConfig::SetBool("muted_notify_unread", false);
    for (const bool archived : {false, true})
      for (const bool snapshot : {false, true})
      {
        UiModel::Impl model(nullptr);
        incoming(model, "old-alert"); model.Process();
        assert(model.m_Notifications.PendingChats() == 1 && badgeVisible);
        if (snapshot)
        {
          auto event = std::make_shared<NewChatsNotify>("tg");
          event->success = true;
          ChatInfo chat; chat.id = "chat"; chat.isMuted = !archived; chat.isArchived = archived;
          event->chatInfos = {chat};
          model.MessageHandler(event);
        }
        else if (archived)
        {
          auto event = std::make_shared<UpdateArchivedNotify>("tg");
          event->success = true; event->chatId = "chat"; event->isArchived = true;
          model.MessageHandler(event);
        }
        else
        {
          auto event = std::make_shared<UpdateMuteNotify>("tg");
          event->success = true; event->chatId = "chat"; event->isMuted = true;
          model.MessageHandler(event);
        }
        model.Process();
        assert(model.m_Notifications.PendingChats() == 0 && !badgeVisible);
        assert(!model.m_Messages["tg"]["chat"]["old-alert"].isRead); // Suppression is not a read receipt.
      }
    // The opt-in terminal policy may retain muted alerts; archive never does.
    UiConfig::SetBool("muted_notify_unread", true);
    for (const bool snapshot : {false, true})
    {
      UiModel::Impl model(nullptr);
      incoming(model, "opt-in-muted-alert"); model.Process();
      if (snapshot)
      {
        auto event = std::make_shared<NewChatsNotify>("tg"); event->success = true;
        ChatInfo chat; chat.id = "chat"; chat.isMuted = true; event->chatInfos = {chat};
        model.MessageHandler(event);
      }
      else
      {
        auto event = std::make_shared<UpdateMuteNotify>("tg");
        event->success = true; event->chatId = "chat"; event->isMuted = true;
        model.MessageHandler(event);
      }
      model.Process(); assert(model.m_Notifications.PendingChats() == 1 && badgeVisible);
      auto archive = std::make_shared<UpdateArchivedNotify>("tg");
      archive->success = true; archive->chatId = "chat"; archive->isArchived = true;
      model.MessageHandler(archive); model.Process();
      assert(model.m_Notifications.PendingChats() == 0 && !badgeVisible);
    }
    UiConfig::SetBool("muted_notify_unread", false);
    for (const bool snapshot : {false, true})
      for (const bool evicted : {false, true})
      {
        UiModel::Impl model(nullptr);
        incoming(model, "mention-alert", "hello", false, "tg", "chat", true);
        incoming(model, "ordinary-alert"); // Newer than the mention: reading it cannot retire this by time.
        model.Process(); assert(model.m_Notifications.PendingChats() == 1 && badgeVisible);
        if (evicted) { model.m_Messages.clear(); model.m_MessageVec.clear(); }
        if (snapshot)
        {
          auto event = std::make_shared<NewChatsNotify>("tg"); event->success = true;
          ChatInfo chat; chat.id = "chat"; chat.isMuted = true; event->chatInfos = {chat};
          model.MessageHandler(event);
        }
        else
        {
          auto event = std::make_shared<UpdateMuteNotify>("tg");
          event->success = true; event->chatId = "chat"; event->isMuted = true;
          model.MessageHandler(event);
        }
        model.Process(); assert(model.m_Notifications.PendingChats() == 1 && badgeVisible);
        read(model, "mention-alert"); model.Process();
        assert(model.m_Notifications.PendingChats() == 0 && !badgeVisible); // Ordinary alert was purged.
        incoming(model, "late-ordinary"); model.Process();
        assert(model.m_Notifications.PendingChats() == 0);
        incoming(model, "late-mention", "hello", false, "tg", "chat", true); model.Process();
        assert(model.m_Notifications.PendingChats() == 1 && badgeVisible);
        auto archive = std::make_shared<UpdateArchivedNotify>("tg");
        archive->success = true; archive->chatId = "chat"; archive->isArchived = true;
        model.MessageHandler(archive); model.Process();
        assert(model.m_Notifications.PendingChats() == 0 && !badgeVisible);
      }
    std::cout << "PASS: muted mentions survive live/snapshot cleanup and evicted history, ordinary alerts retire, future mention policy stays consistent\n";
    std::cout << "PASS: live/snapshot archive and mute transitions retire pending badges without marking read, while opt-in muted alerts remain\n";
  }
  {
    for (const bool cached : {false, true})
      for (const bool replace : {false, true})
      {
        UiModel::Impl model(nullptr);
        incoming(model, "retained"); model.Process();
        auto event = std::make_shared<NewMessagesNotify>("tg");
        event->success = true; event->chatId = "chat";
        ChatMessage retained = model.m_Messages["tg"]["chat"]["retained"];
        retained.isDeleted = true; retained.isRead = true;
        retained.text = replace ? "[This message was deleted]" : "[This message was deleted]\noriginal text";
        retained.fileInfo = replace ? "" : "retained-attachment";
        event->chatMessages = {retained}; model.MessageHandler(event); model.Process();
        assert(model.m_Notifications.PendingChats() == 0);
        auto stale = retained;
        stale.isDeleted = false; stale.isRead = false; stale.text = "stale original text";
        stale.fileInfo = "stale-attachment";
        event->cached = cached; event->chatMessages = {stale};
        model.MessageHandler(event); model.Process();
        const auto& actual = model.m_Messages["tg"]["chat"].at("retained");
        assert(actual.isDeleted && actual.isRead && actual.text == retained.text &&
          actual.fileInfo == retained.fileInfo);
        assert(model.m_MessageVec["tg"]["chat"].size() == 1 && model.m_Notifications.PendingChats() == 0);
        auto file = std::make_shared<NewMessageFileNotify>("tg");
        file->chatId = "chat"; file->msgId = "retained"; file->fileInfo = "download-completed";
        file->downloadFileAction = DownloadFileActionCacheOnly;
        model.MessageHandler(file);
        assert(model.m_Messages["tg"]["chat"].at("retained").fileInfo ==
          (replace ? "" : "download-completed")); // Retained media stays downloadable; redaction stays empty.
        // The same provider ID in another chat/account must remain independent.
        incoming(model, "retained", "unrelated message", cached, "wa", "other");
        assert(!model.m_Messages["wa"]["other"].at("retained").isDeleted);
      }
    std::cout << "PASS: retained deletion state, text and attachments survive stale live/cache replay without cross-chat/account interference\n";
  }
  if (argc == 3)
  {
    // Actual compiled UiModel -> screen's outer tty -> Kitty -> NSDockTile ACK.
    liveDock = true;
    UiModel::Impl model(nullptr);
    auto settle = [&](bool expected) {
      for (int i = 0; i < 40; ++i)
      {
        model.m_NextBadgeSyncMs = 0;
        model.Process();
        if (badgeVisible == expected && SetKittyBadge(expected)) return;
        usleep(50000);
      }
      assert(false && "Kitty did not acknowledge the real Dock badge");
    };
    for (int cycle = 0; cycle < 5; ++cycle)
    {
      const auto id = "live" + std::to_string(cycle);
      incoming(model, id); settle(true);
      read(model, id); settle(false);
    }
    incoming(model, "live-orphan", "hello", false, "wa");
    incoming(model, "live-newer", "hello", false, "wa");
    settle(true);
    read(model, "live-newer", "", "wa");
    settle(false); // Clear the real Dock badge without a receipt for live-orphan.
    std::cout << "missing older receipt through screen to real Kitty Dock: passed\n";
    std::cout << "production handlers through screen to real Kitty NSDockTile: 5 set/read/clear cycles passed\n";
  }
}
