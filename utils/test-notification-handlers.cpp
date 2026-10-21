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
#include <condition_variable>
#include <deque>
#include <thread>
#include <cassert>
#include <clocale>
#include <iostream>
#include <sqlite_modern_cpp.h>
#include "protocol.h"
#include "protocolutil.h"
#include "apputil.h"
#include "owned_mutex.h"
#define private public
#include "messagecache.h"
#include "../src/uimodel.h"
#include "../src/uihistoryview.h"
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
class ModalReadProtocol : public ReceiptTestProtocol
{
public:
  int reads = 0;
  void SendRequest(std::shared_ptr<RequestMessage> request) override
  {
    assert(request->GetMessageType() == MarkMessageReadRequestType);
    ++reads;
  }
};
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

class HistoricalTestProtocol : public ReceiptTestProtocol
{
public:
  std::vector<MessageType> requests;
  void SendRequest(std::shared_ptr<RequestMessage> request) override
  {
    requests.push_back(request->GetMessageType()); // Record only; never reaches a service.
  }
};

static void TestViewOnceTerminal(const std::string& root)
{
  std::setlocale(LC_ALL, "");
  FILE* output = tmpfile(); FILE* input = tmpfile();
  assert(output && input);
  SCREEN* screen = newterm("xterm-256color", output, input);
  assert(screen); set_term(screen); assert(resizeterm(35, 110) != ERR);
  AppConfig::Init(); AppConfig::SetNum("attachment_prefetch", AttachmentPrefetchAll);
  UiColorConfig::Init();
  const auto path = root + "/synthetic-view-once.jpg";
  FILE* image = fopen(path.c_str(), "wb"); assert(image); fputs("synthetic fixture", image); fclose(image);
  {
    UiModel model;
    auto protocol = std::make_shared<HistoricalTestProtocol>();
    const std::string profile = "WhatsAppMd_viewonce";
    auto& impl = model.m_Impl;
    impl.m_Protocols[profile] = protocol;
    impl.m_CurrentChat = {profile, "chat"}; impl.m_CurrentChatIndex = 0;
    impl.m_ChatInfos[profile]["chat"].id = "chat";
    impl.m_MessageVec[profile]["chat"] = {"photo"};
    impl.m_SelectMessageActive = true;
    UiHistoryView history(UiViewParams(0, 0, 110, 35, true, &model));
    for (const std::string prefix : {"", "[View once] ", "[View-once photo]\n", "[View-once video]\n",
                                    "[View-once voice note]\n", "[View-once video note]\n"})
      for (const FileStatus status : {FileStatusNotDownloaded, FileStatusDownloaded})
      {
        FileInfo file;
        file.fileStatus = status; file.fileId = "synthetic-id"; file.filePath = path;
        file.fileType = "image/jpeg"; file.isViewOnce = prefix.empty();
        ChatMessage message;
        message.id = "photo"; message.senderId = "sender"; message.isRead = true;
        message.text = prefix + "preserved caption";
        message.fileInfo = ProtocolUtil::FileInfoToHex(file);
        impl.m_Messages[profile]["chat"][message.id] = message;
        protocol->requests.clear();
        // Both automatic entry points must be blocked before reaching a provider,
        // including legacy cached messages with no dedicated metadata flag.
        impl.DownloadAttachment(profile, "chat", "photo", file.fileId, DownloadFileActionNone);
        impl.DownloadAttachment(profile, "chat", "photo", file.fileId, DownloadFileActionCacheOnly);
        assert(protocol->requests.empty());
        assert(impl.m_Messages[profile]["chat"].at("photo").fileInfo == message.fileInfo);
        {
          std::lock_guard<owned_mutex> lock(model.m_ModelMutex);
          for (int draw = 0; draw < 2; ++draw) { history.SetDirty(true); history.Draw(); }
        }
        assert(protocol->requests.empty() && !history.m_ThumbnailCommandHasImages);
        std::wstring text;
        for (int y = 0; y < history.m_PaddedH; ++y)
        {
          wchar_t row[111] = {};
          mvwinnwstr(history.m_PaddedWin, y, 0, row, 110); text += row; text += L'\n';
        }
        const auto label = text.find(L"[View-once photo · Ctrl+V to view]");
        assert(label != std::wstring::npos && text.find(L"[View-once", label + 1) == std::wstring::npos);
        assert(text.find(L"[View once]") == std::wstring::npos && text.find(L"preserved caption") != std::wstring::npos);
        // Explicit Open/Save retain their existing user-initiated behavior.
        impl.DownloadAttachment(profile, "chat", "photo", file.fileId, DownloadFileActionOpen);
        impl.DownloadAttachment(profile, "chat", "photo", file.fileId, DownloadFileActionSave);
        assert(protocol->requests.size() == 2 && protocol->requests.front() == DownloadFileRequestType);
      }
    FileInfo ordinary; ordinary.fileStatus = FileStatusNotDownloaded; ordinary.fileId = "normal-photo";
    ordinary.filePath = path; ordinary.fileType = "image/jpeg";
    auto& message = impl.m_Messages[profile]["chat"].at("photo");
    message.text = "normal caption"; message.fileInfo = ProtocolUtil::FileInfoToHex(ordinary);
    protocol->requests.clear();
    impl.DownloadAttachment(profile, "chat", "photo", ordinary.fileId, DownloadFileActionNone);
    assert(protocol->requests.size() == 1); // Ordinary image prefetch is unchanged.
  }
  FileUtil::RmFile(path);
  endwin(); delscreen(screen); fclose(output); fclose(input);
  UiColorConfig::Cleanup(); AppConfig::Cleanup();
  std::cout << "PASS: view-once photo metadata and legacy labels block automatic downloads and cached thumbnails, preserve captions and explicit Open/Save, leave ordinary attachments unchanged\n";
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
    UiModel::Impl model(nullptr);
    const std::string profile = "WhatsAppMd_channel_metadata_test";
    ChatInfo existing;
    existing.id = "existing@g.us"; existing.isPinned = true; existing.isArchived = true;
    existing.lastMessageTime = 12345;
    model.m_ChatInfos[profile][existing.id] = existing;
    model.m_ChatVec = {{profile, existing.id}};
    const auto order = model.m_ChatVec;
    const auto mute = [&](const std::string& id, const std::string& source)
    {
      auto event = std::make_shared<UpdateMuteNotify>(source);
      event->success = true; event->chatId = id; event->isMuted = true;
      model.MessageHandler(event);
    };
    mute("known-later@newsletter", profile);
    assert(model.m_ChatInfos[profile]["known-later@newsletter"].id == "known-later@newsletter");
    assert(model.m_ChatInfos[profile]["known-later@newsletter"].isMuted && model.m_ChatVec == order);
    const auto& preserved = model.m_ChatInfos[profile][existing.id];
    assert(preserved.isPinned && preserved.isArchived && preserved.lastMessageTime == existing.lastMessageTime);
    mute("unknown@g.us", profile);
    mute("unknown@newsletter", "Telegram_channel_metadata_test");
    assert(!model.m_ChatInfos[profile].count("unknown@g.us"));
    assert(!model.m_ChatInfos["Telegram_channel_metadata_test"].count("unknown@newsletter"));
    model.GotoChat({profile, "known-later@newsletter"}, true);
    assert(model.m_ChatInfos[profile]["known-later@newsletter"].isMuted);
  }
  {
    // Covered history is still drawn on resize/focus. Exercise the production
    // read handler for every modal kind, including its real request dispatch.
    UiModel::Impl model(nullptr);
    auto service = std::make_shared<ModalReadProtocol>();
    const auto profile = service->GetProfileId();
    model.m_Protocols[profile] = service;
    model.m_CurrentChat = {profile, "covered"};
    model.m_CurrentChatIndex = 0;
    model.m_TerminalActive = true;
    ChatMessage unread;
    unread.id = "covered-unread"; unread.senderId = "sender";
    model.m_Messages[profile]["covered"][unread.id] = unread;
    for (bool* active : {&model.m_ListDialogActive, &model.m_MessageDialogActive, &model.m_FileListDialogActive})
    {
      *active = true;
      model.MarkRead(profile, "covered", unread.id, true);
      assert(service->reads == 0 && !model.m_Messages[profile]["covered"][unread.id].isRead);
      *active = false;
    }
    model.MarkRead(profile, "covered", unread.id, true);
    assert(service->reads == 1 && model.m_Messages[profile]["covered"][unread.id].isRead);
  }
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
  {
    const bool cacheEnabled = MessageCache::m_CacheEnabled;
    MessageCache::m_CacheEnabled = true;
    for (const std::string profile : {"Telegram_deletedcache", "WhatsApp_deletedcache"})
    {
      auto& db = MessageCache::m_Dbs[profile];
      db.reset(new sqlite::database(":memory:"));
      *db << "CREATE TABLE messages(chatId TEXT,id TEXT,text TEXT,fileInfo TEXT,isRead INT,isDeleted INT);";
      *db << "INSERT INTO messages VALUES('chat','redacted','[This message was deleted]','',1,1),"
        "('chat','retained','[This message was deleted] retained text','original-file',1,1);";
      for (const bool cached : {false, true})
        for (const bool staleLoaded : {false, true})
        {
          UiModel::Impl model(nullptr);
          if (staleLoaded)
          {
            ChatMessage stale; stale.id = "retained"; stale.text = "stale undeleted text";
            model.m_Messages[profile]["chat"][stale.id] = stale;
            model.m_MessageVec[profile]["chat"].push_back(stale.id);
          }
          incoming(model, "retained", "stale undeleted text", cached, profile);
          incoming(model, "redacted", "stale undeleted text", cached, profile);
          model.Process();
          const auto& retained = model.m_Messages[profile]["chat"].at("retained");
          const auto& redacted = model.m_Messages[profile]["chat"].at("redacted");
          assert(retained.isDeleted && retained.isRead && retained.text == "[This message was deleted] retained text" &&
            retained.fileInfo == "original-file");
          assert(redacted.isDeleted && redacted.isRead && redacted.text == "[This message was deleted]" && redacted.fileInfo.empty());
          assert(model.m_Notifications.PendingChats() == 0);
          model.m_Messages.clear(); model.m_MessageVec.clear(); // Evict both restored overlays and replay again.
          incoming(model, "retained", "late provider replay", cached, profile);
          assert(model.m_Messages[profile]["chat"].at("retained").isDeleted);
          incoming(model, "retained", "ordinary other chat", cached, profile, "other");
          assert(!model.m_Messages[profile]["other"].at("retained").isDeleted);
        }
      MessageCache::m_Dbs.erase(profile);
    }
    MessageCache::m_CacheEnabled = cacheEnabled;
    std::cout << "PASS: production cache deletion overlays protect evicted/stale-loaded history from live/cache replay without marking other chats read\n";
  }
  {
    UiModel::Impl model(nullptr);
    const std::string profile = "WhatsAppMd_requestedhistory";
    incoming(model, "pending", "live unread", false, profile);
    model.Process();
    assert(model.m_Notifications.PendingChats() == 1 && badgeVisible);
    const auto beforeBells = bells, beforeClears = clears;
    auto protocol = std::make_shared<HistoricalTestProtocol>();
    model.m_Protocols[profile] = protocol;
    auto& live = model.m_Messages[profile]["chat"].at("pending");
    live.fileInfo = "live-downloaded-file";
    const auto liveTime = live.timeSent;
    auto knownRead = live; knownRead.id = "already-read"; knownRead.isRead = true;
    knownRead.timeSent = liveTime + 1;
    model.m_Messages[profile]["chat"][knownRead.id] = knownRead;
    model.m_MessageVec[profile]["chat"].push_back(knownRead.id);
    ChatInfo next; next.id = "next"; next.lastMessageTime = 9000;
    ChatInfo current; current.id = "chat"; current.lastMessageTime = 8000; current.isUnread = true;
    model.m_ChatInfos[profile]["chat"] = current;
    model.m_ChatInfos[profile]["next"] = next;
    model.m_ChatVec = {{profile, "next"}, {profile, "chat"}};
    model.m_ChatSet[profile] = {"next", "chat"};
    model.m_CurrentChat = {profile, "chat"}; model.m_CurrentChatIndex = 1;
    model.m_SelectMessageActive = true; model.m_MessageOffset[profile]["chat"] = 0;
    model.m_HistoryInteraction = true; model.m_HomeFetchAll = true;
    const auto order = model.m_ChatVec;
    auto event = std::make_shared<NewMessagesNotify>(profile);
    event->success = true; event->chatId = "chat"; event->historical = true;
    event->sequence = true; event->cached = false;
    ChatMessage historical = live; historical.id = "old-history";
    historical.timeSent = 10; historical.isRead = true;
    ChatMessage staleUnread = live; staleUnread.isRead = true; staleUnread.timeSent = liveTime + 1000;
    staleUnread.fileInfo = "older-cache-file";
    ChatMessage staleRead = knownRead; staleRead.isRead = false;
    event->chatMessages = {historical, staleUnread, staleRead};
    model.MessageHandler(event); model.Process();
    assert(model.m_Notifications.PendingChats() == 1 && badgeVisible);
    assert(bells == beforeBells && clears == beforeClears);
    assert(!model.m_Notifications.IsRead({profile, "chat"}, "pending"));
    const auto& restored = model.m_Messages[profile]["chat"].at("pending");
    assert(!restored.isRead && restored.timeSent == liveTime && restored.fileInfo == "live-downloaded-file");
    assert(model.m_Messages[profile]["chat"].at("already-read").isRead);
    assert(model.m_ChatVec == order && model.m_CurrentChat == std::make_pair(profile, std::string("chat")));
    assert(model.m_CurrentChatIndex == 1 && model.m_SelectMessageActive && model.m_HistoryInteraction && model.m_HomeFetchAll);
    assert(model.m_MessageVec[profile]["chat"].at(model.m_MessageOffset[profile]["chat"]) == "pending");
    assert(model.m_MessageVec[profile]["chat"].back() == "old-history");
    assert(model.m_OldestMessageId[profile]["chat"] == "old-history" && model.m_OldestMessageTime[profile]["chat"] == 10);
    assert(model.m_ChatInfos[profile]["chat"].isUnread && !model.m_ChatInfos[profile]["chat"].isArchived);
    assert(model.m_ChatInfos[profile]["chat"].lastMessageTime == current.lastMessageTime);
    // Requested history in next, unknown and archived chats must not start
    // provider lookups, pagination cascades or change current list metadata.
    for (const std::string chat : {"next", "unknown", "archived"})
    {
      if (chat == "archived")
      {
        auto archived = current; archived.id = chat; archived.isArchived = true;
        model.m_ChatInfos[profile][chat] = archived;
      }
      event->chatId = chat; event->chatMessages = {historical};
      model.MessageHandler(event);
      assert(model.m_Messages[profile][chat].count("old-history"));
    }
    assert(!model.m_ChatInfos[profile].count("unknown"));
    assert(model.m_ChatInfos[profile]["archived"].isArchived);
    assert(model.m_ChatVec == order && protocol->requests.empty());
    assert(model.m_HistoryInteraction && model.m_HomeFetchAll && model.m_Notifications.PendingChats() == 1);
    std::cout << "PASS: requested historical snapshots preserve live read/download state, unread badges, chat order/metadata and interaction while storing sorted history without provider requests\n";
  }
  TestViewOnceTerminal(argv[1]);
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
