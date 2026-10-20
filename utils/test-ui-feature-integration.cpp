// Offline integration test linked with the built nchat target objects, except
// main.cpp.o and uicontroller.cpp.o. The only substituted component is input:
// all models, dialogs, rendering, local state and cache queries are production.
#include <cassert>
#include <chrono>
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

    Choose("Search cached messages", "Search all chats");
    input.push_back({"Message contains", 0});
    Text("unique-preview-text");
    Key("", "ok");
    Key("Sender name", "ok");
    Key("Type: all", "ok");
    Key("From date", "ok");
    Key("Through date", "ok");
    Choose("Review: Search all chats", "Confirm: Search all chats");
    Key("Search results", "end");
    Key("", "ok");
    Key("Preview must not read this message.", "cancel");
    Run(model, "global_search");
    assert(Chat(model) == "alpha" && Entry(model) == L"Draft alpha ");

    Choose("Chat folders", "Create folder");
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
  std::cout << "PASS: F2 cancel, F3 cached full preview/no read receipt, F4 folder membership, F5 bookmark preview, F6 canceled deletion, per-chat drafts across model reload, deferred transcription source/defer/consume; no service mutations or device access\n";
}
