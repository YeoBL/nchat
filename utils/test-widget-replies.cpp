// Integration test: real file bridge + real temporary cache + fake protocol.
// No user profiles, UI selection, read-receipt requests, or network access.
#include <cassert>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sys/time.h>
#include "appconfig.h"
#include "widgetreplies.h"

struct FakeProtocol : Protocol {
  std::atomic<int> calls{0};
  std::atomic<int> reads{0};
  std::atomic<bool> success{true};
  std::string profile, chat, text;
  explicit FakeProtocol(std::string p) : profile(std::move(p)) {}
  std::string GetProfileId() const override { return profile; }
  std::string GetProfileDisplayName() const override { return profile; }
  bool HasFeature(ProtocolFeature) const override { return false; }
  bool IsGroupChat(const std::string&) const override { return false; }
  std::string GetSelfId() const override { return "self"; }
  bool SetupProfile(const std::string&, std::string&) override { return false; }
  bool LoadProfile(const std::string&, const std::string&) override { return false; }
  bool CloseProfile() override { return true; }
  bool Login() override { return false; }
  bool Logout() override { return true; }
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>&) override {}
  void SendRequest(std::shared_ptr<RequestMessage> request) override {
    if (request->GetMessageType() == MarkMessageReadRequestType) {
      auto read = std::static_pointer_cast<MarkMessageReadRequest>(request);
      assert(read->chatId == "right-chat" && read->msgId == "reference" && read->senderId == "sender");
      assert(!read->readAllReactions);
      read->onResult(success);
      ++reads;
      return;
    }
    assert(request->GetMessageType() == SendMessageRequestType);
    auto send = std::static_pointer_cast<SendMessageRequest>(request);
    assert(send->chatMessage.quotedId.empty()); // Normal reply, not an unintended quote.
    assert(send->chatMessage.fileInfo.empty());
    chat = send->chatId; text = send->chatMessage.text;
    send->onResult(success);
    ++calls;
  }
};

static std::string Read(const std::string& path) {
  std::ifstream file(path);
  return std::string(std::istreambuf_iterator<char>(file), {});
}
static void Write(const std::string& path, const std::string& data) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  assert(fd >= 0 && write(fd, data.data(), data.size()) == (ssize_t)data.size());
  close(fd);
}
static void Await(const std::function<bool()>& ready) {
  for (int i=0; i<150 && !ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(ready());
}

int main() {
  char temporary[] = "/tmp/nchat-widget-replies.XXXXXX";
  const char* root = mkdtemp(temporary);
  assert(root);
  FileUtil::SetApplicationDir(root);
  AppConfig::Init(); MessageCache::Init();
  const std::string profile = "Telegram_test";
  MessageCache::AddProfile(profile, false, 1, false, false);
  ChatMessage message;
  message.id = "reference"; message.senderId = "sender"; message.text = "cached incoming";
  message.timeSent = 1000; message.isOutgoing = false; message.isRead = false;
  MessageCache::AddMessages(profile, "right-chat", "", {message});
  std::vector<ChatMessage> cached;
  Await([&] { cached.clear(); return MessageCache::GetOneMessage(profile, "right-chat", "reference", cached); });
  auto fake = std::make_shared<FakeProtocol>(profile);
  const std::string waProfile = "WhatsAppMd_test";
  MessageCache::AddProfile(waProfile, false, 1, false, false);
  MessageCache::AddMessages(waProfile, "right-chat", "", {message});
  Await([&] { cached.clear(); return MessageCache::GetOneMessage(waProfile, "right-chat", "reference", cached); });
  auto waFake = std::make_shared<FakeProtocol>(waProfile);
  const std::string dir = std::string(root) + "/widget-replies";
  {
    WidgetReplies server({{profile, fake}, {waProfile, waFake}});
    std::istringstream marker(Read(dir + "/server"));
    std::string pid, nonce; marker >> pid >> nonce;
    assert(nonce.size() == 32);
    auto path = [&](int id) { std::ostringstream out; out << dir << "/" << std::setfill('0') << std::setw(32) << std::hex << id; return out.str(); };
    auto payload = [&](const std::string& chat, const std::string& text, const std::string& session) {
      return session + "\n" + StrUtil::StrToHex(profile) + "\n" + StrUtil::StrToHex(chat) + "\n" +
        StrUtil::StrToHex("reference") + "\n" + StrUtil::StrToHex(text) + "\n";
    };
    const std::string text = "Hello 🐢\nQuotes ' \" and $(not a command)";
    Write(path(1) + ".request", payload("right-chat", text, nonce));
    Await([&] { return fake->calls == 1; });
    assert(fake->chat == "right-chat" && fake->text == text);
    assert(Read(path(1) + ".result") == "accepted\n");
    Write(path(1) + ".request", payload("right-chat", text, nonce));
    Await([&] { return access((path(1) + ".request").c_str(), F_OK) != 0; });
    assert(fake->calls == 1); // Duplicate request IDs cannot submit twice.
    for (int id : {2, 3, 4, 5, 6}) {
      std::string body = payload(id == 2 ? "wrong-chat" : "right-chat",
        id == 3 ? " \t\n" : id == 4 ? std::string(4001, 'x') : id == 6 ? std::string("\xff", 1) : text,
        id == 5 ? std::string(32, '0') : nonce);
      Write(path(id) + ".request", body);
      Await([&] { return Read(path(id) + ".result") == "rejected\n"; });
    }
    Write(path(7) + ".request", payload("right-chat", text, nonce));
    timeval old[2] = {{time(nullptr)-60,0},{time(nullptr)-60,0}};
    utimes((path(7) + ".request").c_str(), old);
    Await([&] { return Read(path(7) + ".result") == "rejected\n"; });
    assert(fake->calls == 1);
    fake->success = false;
    Write(path(8) + ".request", payload("right-chat", "failure test", nonce));
    Await([&] { return fake->calls == 2; });
    assert(Read(path(8) + ".result") == "failed\n");
    cached.clear();
    assert(MessageCache::GetOneMessage(profile, "right-chat", "reference", cached));
    assert(!cached.front().isRead);
    fake->success = true;
    Write(path(10) + ".read", payload("right-chat", "mark-read", nonce));
    Await([&] { return Read(path(10) + ".result") == "accepted\n"; });
    assert(fake->reads == 1 && fake->calls == 2);
    Write(path(10) + ".read", payload("right-chat", "mark-read", nonce));
    Await([&] { return access((path(10) + ".read").c_str(), F_OK) != 0; });
    assert(fake->reads == 1);
    Write(path(11) + ".read", payload("wrong-chat", "mark-read", nonce));
    Await([&] { return Read(path(11) + ".result") == "rejected\n"; });
    Write(path(12) + ".read", payload("right-chat", "reply must not become a read", nonce));
    Await([&] { return Read(path(12) + ".result") == "rejected\n"; });
    fake->success = false;
    Write(path(13) + ".read", payload("right-chat", "mark-read", nonce));
    Await([&] { return Read(path(13) + ".result") == "failed\n"; });
    assert(fake->reads == 2 && fake->calls == 2);
    const std::string waPayload = nonce + "\n" + StrUtil::StrToHex(waProfile) + "\n" +
      StrUtil::StrToHex("right-chat") + "\n" + StrUtil::StrToHex("reference") + "\n" + StrUtil::StrToHex("mark-read") + "\n";
    Write(path(14) + ".read", waPayload);
    Await([&] { return Read(path(14) + ".result") == "accepted\n"; });
    assert(waFake->reads == 1 && waFake->calls == 0 && fake->reads == 2);
    cached.clear();
    assert(MessageCache::GetOneMessage(profile, "right-chat", "reference", cached) && !cached.front().isRead);
    std::cout << "PASS: explicit mark-read uses exact chat/message/sender, no text send, no cache rewrite, duplicates rejected and failures acknowledged\n";
  }
  assert(access((dir + "/server").c_str(), F_OK) != 0);
  MessageCache::Cleanup(); AppConfig::Cleanup();
  FileUtil::RmDir(root);
  std::cout << "PASS: exact recipient/text, Unicode/newlines, no receipt requests, unchanged cache read flag, duplicate prevention, stale sessions, expired requests, invalid text, failures and shutdown\n";
}
