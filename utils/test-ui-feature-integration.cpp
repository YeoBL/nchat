// Offline integration test linked with the built nchat target objects, except
// main.cpp.o and uicontroller.cpp.o. The only substituted component is input:
// all models, dialogs, rendering, local state and cache queries are production.
#include <cassert>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <thread>

#include <ncurses.h>
#include <unistd.h>

#include "appconfig.h"
#include "fileutil.h"
#include "localfeatures.h"
#include "messagecache.h"
#include "protocolutil.h"
#include "strutil.h"
#include "uicolorconfig.h"
#include "uiconfig.h"
#include "uicontroller.h"
#include "uikeyconfig.h"
#include "uimodel.h"

static_assert(DownloadFileActionTranscribe != DownloadFileActionCacheOnly,
  "Transcription downloads must not be swallowed by the read-only cache action");

namespace {
const std::string Profile = "Telegram_offline_ui_test";
struct Step { std::string screen; wint_t key; };
std::deque<Step> input;
int waits = 0;

std::string ScreenText()
{
  std::string text;
  for (int y = 0; y < LINES; ++y)
  {
    for (int x = 0; x < COLS; ++x)
    {
      const auto c = mvwinch(curscr, y, x) & A_CHARTEXT;
      text += c >= 32 && c < 127 ? static_cast<char>(c) : ' ';
    }
    text += '\n';
  }
  return text;
}

void Key(const std::string& screen, const std::string& name)
{
  input.push_back({screen, static_cast<wint_t>(UiKeyConfig::GetKey(name))});
}
void Text(const std::string& text)
{
  for (const auto c : text) input.push_back({"", static_cast<wint_t>(c)});
}
void Choose(const std::string& screen, const std::string& filter)
{
  // Filter the real list, then End selects the item's final wrapped detail row.
  input.push_back({screen, 0});
  // Restored pages retain their last list filter, including after canceling a
  // field editor. Clear it before choosing a different action on that page.
  for (int i = 0; i < 100; ++i) Key("", "backspace");
  Text(filter);
  Key("", "end");
  Key("", "ok");
}
void Run(UiModel& model, const std::string& shortcut)
{
  waits = 0;
  model.KeyHandler(UiKeyConfig::GetKey(shortcut));
  assert(input.empty());
}

struct FakeProtocol : Protocol
{
  std::vector<MessageType> requests;
  std::string GetProfileId() const override { return Profile; }
  std::string GetProfileDisplayName() const override { return "Offline test"; }
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
    requests.push_back(request->GetMessageType());
    // This test deliberately never contacts Telegram or launches native helpers.
    if (request->GetMessageType() == ClientActionRequestType)
    {
      auto action = std::static_pointer_cast<ClientActionRequest>(request);
      ClientActionResult page;
      page.title = "Offline account tools";
      action->onResult(page);
    }
  }
  void AssertNoMutations() const
  {
    for (const auto type : requests)
      assert(type != SendMessageRequestType && type != MarkMessageReadRequestType &&
             type != DeleteMessageRequestType && type != ClientActionRequestType);
  }
};

struct EditingProtocol : FakeProtocol
{
  explicit EditingProtocol(std::string id = Profile) : profile(std::move(id)) {}
  std::string GetProfileId() const override { return profile; }
  bool HasFeature(ProtocolFeature feature) const override { return feature == FeatureEditMessagesWithinTwoDays; }
  void SendRequest(std::shared_ptr<RequestMessage> request) override
  {
    if (request->GetMessageType() == EditMessageRequestType)
      edits.push_back(std::static_pointer_cast<EditMessageRequest>(request));
    if (request->GetMessageType() == ClientActionRequestType && !composeTarget.empty())
    {
      requests.push_back(request->GetMessageType());
      auto action = std::static_pointer_cast<ClientActionRequest>(request);
      ClientActionResult page;
      page.title = "Offline cloud draft";
      ClientActionItem item;
      item.title = "Load into textbox";
      item.operation = "local.compose";
      item.params = {{"profile", profile}, {"chatId", composeTarget}, {"text", composeText}};
      page.items.push_back(item);
      action->onResult(page);
      return;
    }
    FakeProtocol::SendRequest(request);
  }
  std::string profile, composeTarget, composeText;
  std::vector<std::shared_ptr<EditMessageRequest>> edits;
};

struct HistoryProtocol : FakeProtocol
{
  const std::string profile = "WhatsAppMd_offline_history_test";
  std::vector<std::shared_ptr<ClientActionRequest>> historyRequests;
  std::string GetProfileId() const override { return profile; }
  void SendRequest(std::shared_ptr<RequestMessage> request) override
  {
    assert(request->GetMessageType() != SendMessageRequestType &&
      request->GetMessageType() != MarkMessageReadRequestType);
    if (request->GetMessageType() != ClientActionRequestType) return;
    auto action = std::static_pointer_cast<ClientActionRequest>(request);
    assert(action->operation == "history_request");
    historyRequests.push_back(action);
    ClientActionResult result;
    result.title = "Synthetic history requested";
    result.text = "Offline request acknowledgement only.";
    action->onResult(result);
  }
};

void Populate(UiModel& model, const std::shared_ptr<FakeProtocol>& protocol)
{
  model.AddProtocol(protocol);
  model.Init();
  auto contacts = std::make_shared<NewContactsNotify>(Profile);
  for (const auto& id : {"alpha", "beta"})
  {
    ContactInfo contact;
    contact.id = id;
    contact.name = std::string("Test ") + id;
    contacts->contactInfos.push_back(contact);
  }
  model.MessageHandler(contacts);
  auto chats = std::make_shared<NewChatsNotify>(Profile);
  chats->success = true;
  for (const auto& id : {"alpha", "beta"})
  {
    ChatInfo chat;
    chat.id = id;
    chat.lastMessageTime = std::string(id) == "alpha" ? 2000 : 1000;
    chats->chatInfos.push_back(chat);
  }
  model.MessageHandler(chats);
  auto messages = std::make_shared<NewMessagesNotify>(Profile);
  messages->success = true;
  messages->chatId = "alpha";
  messages->cached = true;
  ChatMessage message;
  message.id = "loaded-message";
  message.senderId = "alpha";
  message.text = "Loaded synthetic message for safe bulk selection";
  message.timeSent = 2000;
  message.isRead = true;
  messages->chatMessages = {message};
  model.MessageHandler(messages);
}

// Locked getters are safe here: this model is accessed by one test thread only,
// and the fake service never calls it back or starts any service threads.
std::wstring Entry(UiModel& model) { return model.GetEntryStrLocked(); }
std::string Chat(UiModel& model) { return model.GetCurrentChatLocked().second; }
void Type(UiModel& model, const std::wstring& text)
{
  for (const auto c : text) model.KeyHandler(c);
}
}

UiController::UiController() {}
UiController::~UiController() {}
void UiController::Init() {}
void UiController::Cleanup() {}
wint_t UiController::GetKey(int)
{
  assert(!input.empty() && "Dialog requested unexpected input");
  const auto& next = input.front();
  if (!next.screen.empty() && ScreenText().find(next.screen) == std::string::npos)
  {
    if (++waits > 1000)
    {
      std::cerr << "Missing screen text: " << next.screen << "\n" << ScreenText();
      std::abort();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return 0;
  }
  waits = 0;
  const auto key = next.key;
  input.pop_front();
  return key;
}

int main()
{
  std::setlocale(LC_ALL, ""); // Match Ui::Init(), including multibyte emoji conversion.
  char temp[] = "/tmp/nchat-ui-features.XXXXXX";
  const auto root = mkdtemp(temp);
  assert(root);
  FileUtil::SetApplicationDir(root);
  FileUtil::InitTempDir();
  unsetenv("NCHAT_NO_AUTO_SELECT");
  // Dialogs clear terminal thumbnails through the configured command. Use the
  // system no-op, never any real thumbnail, recording or transcription helper.
  {
    std::ofstream config(std::string(root) + "/ui.conf");
    config << "thumbnail_command=/usr/bin/true\n";
  }
  AppConfig::Init();
  UiConfig::Init();
  UiConfig::SetBool("desktop_notify_enabled", false);
  UiConfig::SetBool("terminal_bell_active", false);
  UiConfig::SetBool("terminal_bell_inactive", false);
  UiConfig::SetBool("typing_status_share", false);
  MessageCache::Init();
  MessageCache::AddProfile(Profile, false, 1, false, false);
  ChatMessage unread;
  unread.id = "off-chat-unread";
  unread.senderId = "test-sender";
  unread.text = "unique-preview-text with a complete second line\nPreview must not read this message.";
  unread.timeSent = 1500;
  unread.isRead = false;
  MessageCache::AddMessages(Profile, "cache-only-chat", "", {unread});
  std::vector<ChatMessage> cached;
  for (int i = 0; i < 100 && !MessageCache::GetOneMessage(Profile, "cache-only-chat", unread.id, cached); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(!cached.empty());
  {
    LocalFeatures local;
    assert(local.SetBookmark({Profile, "cache-only-chat"}, unread.id, unread.text, unread.timeSent));
  }

  FILE* output = tmpfile();
  FILE* source = tmpfile();
  assert(output && source);
  SCREEN* terminal = newterm("xterm-256color", output, source);
  assert(terminal);
  set_term(terminal);
  assert(resizeterm(45, 140) != ERR);
  UiColorConfig::Init();
  UiKeyConfig::Init(false);
  assert(UiKeyConfig::GetStr("client_actions") == "KEY_F2");
  assert(UiKeyConfig::GetStr("global_search") == "KEY_F3");
  assert(UiKeyConfig::GetStr("folders") == "KEY_F4");
  assert(UiKeyConfig::GetStr("bookmarks") == "KEY_F5");
  assert(UiKeyConfig::GetStr("bulk_select") == "KEY_F6");
  auto protocol = std::make_shared<FakeProtocol>();
  {
    UiModel model;
    Populate(model, protocol);
    Type(model, L"Draft alpha ");
    assert(Chat(model) == "alpha");
    model.KeyHandler(UiKeyConfig::GetKey("next_chat"));
    assert(Chat(model) == "beta");
    Type(model, L"Draft beta");
    model.KeyHandler(UiKeyConfig::GetKey("prev_chat"));
    assert(Chat(model) == "alpha" && Entry(model) == L"Draft alpha ");

    Key("Search messages across chats", "cancel");
    Run(model, "client_actions");

    input.push_back({"Message contains", 0});
    Text("unique-preview-text");
    Key("", "ok");
    // One text submission must go straight to results: any old scope menu,
    // sender/type/date wizard or review page fails this screen assertion.
    Choose("Search results", "unique-preview-text");
    Key("Preview must not read this message.", "cancel");
    Run(model, "global_search");
    assert(Chat(model) == "alpha" && Entry(model) == L"Draft alpha ");

    // Filters are optional, independently editable and survive Back/cancel.
    Key("Message contains", "ok"); // blank query: show recent messages
    Choose("Search results", "Filters (optional)");
    Choose("Search filters", "Sender: Any");
    Text("not-a-matching-sender");
    Key("", "ok"); // no other field prompts and no review
    Choose("Sender: not-a-matching-sender", "From date: Any");
    Text("2000-01-01");
    Key("", "cancel");
    Choose("From date: Any", "Apply filters");
    Choose("Search results", "Edit search");
    Text("changed but canceled");
    Key("", "cancel");
    Choose("Search results", "Filters (optional)");
    Key("Sender: not-a-matching-sender", "left");
    Choose("Search results", "Clear filters");
    Choose("Search results", "unique-preview-text");
    Key("Preview must not read this message.", "left");
    Choose("Search results", "Edit search");
    Text("no-matches-text");
    Key("", "ok");
    Choose("Search results", "Edit search");
    Key("Message contains", "delete_line_before_cursor");
    Text("unique-preview-text");
    Key("", "ok");
    Choose("Search results", "unique-preview-text");
    Key("Preview must not read this message.", "cancel");
    Run(model, "global_search");

    // The F2 entry shares the query-only UX, rather than restoring the wizard.
    Choose("Search messages across chats", "Search messages across chats");
    Text("unique-preview-text");
    Key("", "ok");
    Choose("Search results", "unique-preview-text");
    Key("Preview must not read this message.", "cancel");
    Run(model, "client_actions");

    Choose("Chat folders", "Create folder");
    Choose("Review: Create folder", "Folder name");
    input.push_back({"Folder name", 0});
    Text("Offline friends");
    Key("", "ok");
    Choose("Review: Create folder", "Confirm: Create folder");
    Choose("Chat folders", "Offline friends");
    Choose("Add current chat to folder", "Add current chat to folder");
    Key("Remove current chat from folder", "cancel");
    Run(model, "folders");
    {
      LocalFeatures local;
      assert(local.InFolder({Profile, "alpha"}, "Offline friends"));
    }

    Key("Local bookmarks remain", "end");
    Key("", "ok");
    Key("Preview must not read this message.", "cancel");
    Run(model, "bookmarks");

    Choose("Selected messages", "Select all loaded messages");
    Choose("Delete selected messages", "Delete selected messages");
    Key("Confirm action", "cancel");
    Key("Delete selected messages", "cancel");
    Run(model, "bulk_select");
    protocol->AssertNoMutations();
    cached.clear();
    assert(MessageCache::GetOneMessage(Profile, "cache-only-chat", unread.id, cached));
    assert(!cached.front().isRead);

    // A downloaded file can arrive after the user switches to another account
    // or chat. Deferred transcription must retain its source, not the current
    // selection. Inspect the queue without Process(), so no helper is launched.
    const std::string sourceProfile = "WhatsAppMd_other_offline_test";
    const std::string audioPath = std::string(root) + "/synthetic-audio.m4a";
    {
      std::ofstream audio(audioPath);
      audio << "Synthetic fixture: never decoded or sent";
    }
    FileInfo file;
    file.fileStatus = FileStatusDownloaded;
    file.filePath = audioPath;
    file.fileType = "audio/mp4";
    auto downloaded = std::make_shared<NewMessageFileNotify>(sourceProfile);
    downloaded->chatId = "source-chat-not-current";
    downloaded->msgId = "source-message-not-selected";
    downloaded->fileInfo = ProtocolUtil::FileInfoToHex(file);
    downloaded->downloadFileAction = DownloadFileActionTranscribe;
    model.MessageHandler(downloaded);
    assert(Chat(model) == "alpha");
    std::map<std::string, std::string> origin;
    const auto takeTranscription = [&]()
    {
      std::unique_lock<owned_mutex> lock(model.m_ModelMutex);
      return model.GetImpl().TakePendingTranscription(origin);
    };
    model.SetListDialogActive(true);
    assert(takeTranscription().empty());
    assert(origin.empty());
    model.SetListDialogActive(false);
    assert(takeTranscription() == audioPath);
    assert(origin.at("profile") == sourceProfile);
    assert(origin.at("chatId") == downloaded->chatId);
    assert(origin.at("msgId") == downloaded->msgId);
    assert(takeTranscription().empty());
    model.Cleanup();
  }
  {
    UiModel model;
    Populate(model, protocol);
    model.KeyHandler(UiKeyConfig::GetKey("left")); // Select initial chat without editing.
    assert(Chat(model) == "alpha" && Entry(model) == L"Draft alpha ");
    model.KeyHandler(UiKeyConfig::GetKey("next_chat"));
    assert(Chat(model) == "beta" && Entry(model) == L"Draft beta");
    model.Cleanup();
  }
  protocol->AssertNoMutations();

  // Selected-message recovery must anchor after the target's provider second,
  // not on the target itself or a subsecond cache tie-breaker. This exercises
  // the actual UI dispatch and cache without contacting a phone or marking read.
  {
    auto history = std::make_shared<HistoryProtocol>();
    MessageCache::AddProfile(history->profile, false, 1, false, false);
    ChatMessage target;
    target.id = "selected"; target.timeSent = 10099;
    ChatMessage sameSecond = target;
    sameSecond.id = "same-second"; sameSecond.timeSent = 10999;
    ChatMessage later = target;
    later.id = "later-anchor"; later.timeSent = 12099; later.isOutgoing = true;
    later.senderId = "self";
    ChatMessage localGroupEvent = target;
    localGroupEvent.id = "11"; localGroupEvent.timeSent = 11050;
    MessageCache::AddMessages(history->profile, "history-chat", "", {target, sameSecond, localGroupEvent, later});
    std::vector<ChatMessage> stored;
    for (int i = 0; i < 100 && !MessageCache::GetOneMessage(history->profile, "history-chat", later.id, stored); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!stored.empty());
    UiModel model;
    model.AddProtocol(history); model.Init();
    const std::map<std::string, std::string> values = {{"profile", history->profile},
      {"chatId", "history-chat"}, {"msgId", target.id}, {"timeSent", std::to_string(target.timeSent)}};
    Key("Synthetic history requested", "cancel");
    model.OnKeyClientActions("local.whatsapp.history", values);
    assert(input.empty() && history->historyRequests.size() == 1);
    const auto& sent = history->historyRequests.front()->params;
    assert(sent.at("msgId") == later.id && sent.at("timeSent") == "12099" && sent.at("isOutgoing") == "1");
    assert(sent.at("targetMsgId") == target.id && sent.at("targetTimeSent") == "10099");
    auto noAnchor = values;
    noAnchor["msgId"] = later.id; noAnchor["timeSent"] = "12099";
    Key("No later cached message", "cancel");
    model.OnKeyClientActions("local.whatsapp.history", noAnchor);
    assert(input.empty() && history->historyRequests.size() == 1);
    model.Cleanup();
  }

  // Editing a sent message is a temporary composer mode. Cancel, save, cloud
  // draft loading, navigation and disappearing history must never turn its
  // contents into a new chat's edit or destroy the original unsent draft.
  {
    auto editing = std::make_shared<EditingProtocol>();
    auto other = std::make_shared<EditingProtocol>("WhatsAppMd_offline_edit_test");
    UiModel model;
    Populate(model, editing);
    model.AddProtocol(other);
    auto& impl = model.GetImpl();
    auto originalMessage = impl.GetMessages(Profile, "alpha").at("loaded-message");
    originalMessage.isOutgoing = true;
    originalMessage.timeSent = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    impl.GetMessages(Profile, "alpha")[originalMessage.id] = originalMessage;
    const std::string originalDraft = "Unsent Unicode draft Ω\nSecond line";
    const int originalCursor = 7;
    impl.ReplaceDraft(originalDraft);
    impl.GetEntryPos() = originalCursor;
    const auto beginEdit = [&]() {
      impl.SetSelectMessageActive(true);
      impl.GetMessageOffset(Profile, "alpha") = 0;
      model.KeyHandler(UiKeyConfig::GetKey("edit_msg"));
      assert(impl.GetEditMessageActive());
      assert(Entry(model) == StrUtil::ToWString(originalMessage.text));
    };
    const auto restored = [&]() {
      assert(!impl.GetEditMessageActive());
      assert(Entry(model) == StrUtil::ToWString(originalDraft));
      assert(impl.GetEntryPos() == originalCursor);
      assert(impl.GetLocalFeatures().GetDraft({Profile, "alpha"}) == originalDraft);
    };

    beginEdit();
    Type(model, L" changed then cancelled");
    model.KeyHandler(UiKeyConfig::GetKey("cancel"));
    restored();
    assert(impl.GetLocalFeatures().Flush());
    { LocalFeatures disk; assert(disk.GetDraft({Profile, "alpha"}) == originalDraft); }

    beginEdit();
    Type(model, L" amended");
    const auto editedText = StrUtil::ToString(Entry(model));
    model.KeyHandler(UiKeyConfig::GetKey("send_msg"));
    assert(editing->edits.size() == 1);
    assert(editing->edits.back()->chatId == "alpha" && editing->edits.back()->msgId == originalMessage.id);
    assert(editing->edits.back()->chatMessage.text == editedText);
    restored();

    beginEdit();
    impl.GetMessages(Profile, "alpha").erase(originalMessage.id);
    model.KeyHandler(UiKeyConfig::GetKey("send_msg"));
    restored();
    assert(editing->edits.size() == 1);
    impl.GetMessages(Profile, "alpha")[originalMessage.id] = originalMessage;
    beginEdit();
    impl.GetMessages(Profile, "alpha")[originalMessage.id].isDeleted = true;
    model.KeyHandler(UiKeyConfig::GetKey("send_msg"));
    restored();
    assert(editing->edits.size() == 1);
    impl.GetMessages(Profile, "alpha")[originalMessage.id] = originalMessage;

    // Emoji display is global. Toggling it during an edit must transform the
    // suspended draft too, preserving its logical cursor through both formats.
    if (!impl.GetEmojiEnabled()) model.KeyHandler(UiKeyConfig::GetKey("toggle_emoji"));
    const std::string textDraft = "Before :smile: after";
    const auto emojiDraft = StrUtil::ToWString(StrUtil::Emojize(textDraft, true));
    assert(emojiDraft != StrUtil::ToWString(textDraft));
    const int emojiCursor = static_cast<int>(StrUtil::ToWString(StrUtil::Emojize("Before :smile:", true)).size());
    impl.ReplaceDraft(StrUtil::ToString(emojiDraft));
    impl.GetEntryPos() = emojiCursor;
    beginEdit();
    model.KeyHandler(UiKeyConfig::GetKey("toggle_emoji"));
    model.KeyHandler(UiKeyConfig::GetKey("cancel"));
    assert(!impl.GetEmojiEnabled() && !impl.GetEditMessageActive());
    assert(Entry(model) == StrUtil::ToWString(textDraft));
    assert(impl.GetEntryPos() == static_cast<int>(std::string("Before :smile:").size()));
    beginEdit();
    model.KeyHandler(UiKeyConfig::GetKey("toggle_emoji"));
    model.KeyHandler(UiKeyConfig::GetKey("cancel"));
    assert(impl.GetEmojiEnabled() && !impl.GetEditMessageActive());
    assert(Entry(model) == emojiDraft && impl.GetEntryPos() == emojiCursor);
    // Saving after a display-mode change also restores the converted draft.
    beginEdit();
    model.KeyHandler(UiKeyConfig::GetKey("toggle_emoji"));
    model.KeyHandler(UiKeyConfig::GetKey("send_msg"));
    assert(editing->edits.size() == 2 && !impl.GetEditMessageActive());
    assert(Entry(model) == StrUtil::ToWString(textDraft));
    assert(impl.GetEntryPos() == static_cast<int>(std::string("Before :smile:").size()));
    model.KeyHandler(UiKeyConfig::GetKey("toggle_emoji"));
    impl.ReplaceDraft(originalDraft);
    impl.GetEntryPos() = originalCursor;
    restored();

    impl.GotoChat({Profile, "beta"});
    impl.ReplaceDraft("Unchanged beta draft");
    impl.GetEntryPos() = 3;
    impl.GotoChat({Profile, "alpha"});
    restored();
    assert(impl.GetLocalFeatures().SetFolder({Profile, "beta"}, "Edit navigation target", true));
    beginEdit();
    Choose("Chat folders", "Edit navigation target");
    Choose("Edit navigation target", "Test beta");
    Run(model, "folders");
    assert(Chat(model) == "beta" && !impl.GetEditMessageActive());
    assert(Entry(model) == L"Unchanged beta draft" && impl.GetEntryPos() == 3);
    impl.SaveEditMessage(); // No stale ID lookup or outgoing request after navigation.
    assert(editing->edits.size() == 2);
    impl.GotoChat({Profile, "alpha"});
    restored();

    assert(impl.GetLocalFeatures().SetDraft({other->profile, "other-chat"}, "Other account draft"));
    beginEdit();
    impl.GotoChat({other->profile, "other-chat"}, true);
    assert(!impl.GetEditMessageActive() && Entry(model) == L"Other account draft");
    impl.GotoChat({Profile, "alpha"});
    restored();

    // Compose replacement is reviewed before navigation. A cancelled review
    // preserves the current editing session as well as both chats' drafts.
    editing->composeTarget = "beta";
    editing->composeText = "Cloud replacement";
    const auto compose = [&](bool review, bool accept) {
      Choose("Search messages across chats", "Account tools");
      Choose("Offline cloud draft", "Load into textbox");
      if (review) Key("This chat already has an unsent draft.", accept ? "ok" : "cancel");
      Run(model, "client_actions");
    };
    beginEdit();
    const auto editingEntry = Entry(model);
    compose(true, false);
    assert(Chat(model) == "alpha" && impl.GetEditMessageActive() && Entry(model) == editingEntry);
    assert(impl.GetLocalFeatures().GetDraft({Profile, "beta"}) == "Unchanged beta draft");
    compose(true, true);
    assert(Chat(model) == "beta" && !impl.GetEditMessageActive() && Entry(model) == L"Cloud replacement");
    impl.GetEntryPos() = 2;
    compose(false, false); // Identical text: neither prompt nor cursor jump.
    assert(Chat(model) == "beta" && Entry(model) == L"Cloud replacement" && impl.GetEntryPos() == 2);
    editing->composeTarget = "empty-new-chat";
    editing->composeText = "First draft";
    compose(false, false); // Empty destination: no destructive replacement.
    assert(Chat(model) == "empty-new-chat" && Entry(model) == L"First draft");

    impl.GotoChat({Profile, "alpha"});
    restored();
    beginEdit();
    impl.ReplaceDraft(originalDraft); // Same-chat compose also exits edit safely.
    restored();
    beginEdit();
    model.Cleanup(); // Shutdown while editing preserves the ordinary draft.
    restored();
    { LocalFeatures disk; assert(disk.GetDraft({Profile, "alpha"}) == originalDraft); }
    assert(editing->edits.size() == 2);
    for (const auto type : editing->requests)
      assert(type != SendMessageRequestType && type != DeleteMessageRequestType && type != MarkMessageReadRequestType);
  }
  UiKeyConfig::Cleanup();
  UiColorConfig::Cleanup();
  endwin();
  delscreen(terminal);
  fclose(source);
  fclose(output);
  MessageCache::Cleanup();
  UiConfig::Cleanup();
  AppConfig::Cleanup();
  FileUtil::RmDir(root);
  std::cout << "PASS: F2-F6 workflows, persistent drafts, edit cancel/save/navigation/missing-message safety, reviewed cloud-draft replacement and transcription origin; fake protocols only, no real service mutations or device access\n";
}
