// Explicit reply/read actions from the widget. Previewing never uses this bridge.
#pragma once
#include <atomic>
#include <chrono>
#include <codecvt>
#include <fcntl.h>
#include <locale>
#include <random>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include "fileutil.h"
#include "messagecache.h"
#include "strutil.h"

class WidgetReplies
{
public:
  explicit WidgetReplies(const std::unordered_map<std::string, std::shared_ptr<Protocol>>& protocols)
    : m_Protocols(protocols), m_Dir(FileUtil::GetApplicationDir() + "/widget-replies")
  {
    mkdir(m_Dir.c_str(), 0700);
    struct stat st;
    if (lstat(m_Dir.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return;
    m_Lock = open((m_Dir + "/lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (m_Lock < 0 || flock(m_Lock, LOCK_EX | LOCK_NB)) return;
    std::random_device random;
    for (int i = 0; i < 32; ++i) m_Nonce += "0123456789abcdef"[random() % 16];
    if (!Write(m_Dir + "/server", std::to_string(getpid()) + " " + m_Nonce + " read-v1\n")) return;
    m_Thread = std::thread([this] {
      while (!m_Stop)
      {
        for (const auto& name : FileUtil::ListDirNames(m_Dir))
        {
          if (m_Stop) break;
          if (!((name.size() == 40 && name.substr(32) == ".request") ||
                (name.size() == 37 && name.substr(32) == ".read")) ||
              name.substr(0, 32).find_first_not_of("0123456789abcdef") != std::string::npos) continue;
          Process(m_Dir + "/" + name);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });
  }

  ~WidgetReplies()
  {
    m_Stop = true;
    if (m_Thread.joinable()) { m_Thread.join(); unlink((m_Dir + "/server").c_str()); }
    if (m_Lock >= 0) close(m_Lock);
  }

private:
  static bool Write(const std::string& path, const std::string& value)
  {
    std::string temporary = path + ".XXXXXX";
    int fd = mkstemp(&temporary[0]);
    if (fd < 0) return false;
    bool ok = write(fd, value.data(), value.size()) == (ssize_t)value.size();
    if (ok) ok = fsync(fd) == 0;
    close(fd);
    if (ok) ok = rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) unlink(temporary.c_str());
    return ok;
  }

  void Process(const std::string& path)
  {
    const bool markRead = path.size() >= 5 && path.substr(path.size() - 5) == ".read";
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) ||
        st.st_size <= 0 || st.st_size > 40000) { close(fd); return; }
    std::string data((size_t)st.st_size, '\0');
    ssize_t len = read(fd, &data[0], data.size());
    close(fd);
    unlink(path.c_str()); // Claim exactly once, before any network action.
    const std::string result = path.substr(0, path.size() - (markRead ? 5 : 8)) + ".result";
    if (access(result.c_str(), F_OK) == 0) return; // Duplicate request ID never resends.
    auto reject = [&] { Write(result, "rejected\n"); };
    if (len != (ssize_t)data.size() || time(nullptr) - st.st_mtime > 30 || st.st_mtime > time(nullptr) + 5)
      { reject(); return; }
    std::istringstream input(data);
    std::string nonce, fields[4], extra;
    if (!std::getline(input, nonce) || nonce != m_Nonce) { reject(); return; }
    for (auto& field : fields)
    {
      if (!std::getline(input, field) || field.empty() || field.size() % 2 ||
          field.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) { reject(); return; }
      field = StrUtil::StrFromHex(field);
      if (field.find('\0') != std::string::npos) { reject(); return; }
    }
    if (std::getline(input, extra)) { reject(); return; }
    try {
      std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
      const auto text = converter.from_bytes(fields[3]);
      if (text.empty() || text.size() > 4000 || fields[3].find_first_not_of(" \t\r\n") == std::string::npos)
        { reject(); return; }
    } catch (...) { reject(); return; }
    auto protocol = m_Protocols.find(fields[0]);
    if (protocol == m_Protocols.end() ||
        (fields[0].find("Telegram_") != 0 && fields[0].find("WhatsAppMd_") != 0)) { reject(); return; }
    std::vector<ChatMessage> messages;
    if (markRead)
    {
      if (fields[3] != "mark-read") { reject(); return; }
      std::istringstream ids(fields[2]);
      std::string id;
      std::set<std::string> seen;
      while (std::getline(ids, id))
      {
        std::vector<ChatMessage> cached;
        if (id.empty() || !seen.insert(id).second || seen.size() > 100 ||
            !MessageCache::GetOneMessage(fields[0], fields[1], id, cached) || cached.empty() ||
            cached.front().isOutgoing || cached.front().isDeleted) { reject(); return; }
        if (!cached.front().isRead) messages.push_back(cached.front());
      }
      if (seen.empty()) { reject(); return; }
    }
    else if (!MessageCache::GetOneMessage(fields[0], fields[1], fields[2], messages) || messages.empty() || messages.front().isDeleted)
      { reject(); return; }
    // Persist the claim before dispatch. After a crash, ambiguous sends are not retried.
    int claim = open(result.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (claim < 0) return;
    const bool claimed = write(claim, "pending\n", 8) == 8 && fsync(claim) == 0;
    close(claim);
    if (!claimed) return;
    if (markRead)
    {
      if (messages.empty()) { Write(result, "accepted\n"); return; }
      auto remaining = std::make_shared<std::atomic<size_t>>(messages.size());
      auto failed = std::make_shared<std::atomic<bool>>(false);
      for (const auto& message : messages)
      {
        auto request = std::make_shared<MarkMessageReadRequest>();
        request->chatId = fields[1];
        request->msgId = message.id;
        request->senderId = message.senderId;
        request->onResult = [remaining, failed, result](bool success) {
          if (!success) *failed = true;
          if (remaining->fetch_sub(1) == 1) Write(result, *failed ? "failed\n" : "accepted\n");
        };
        protocol->second->SendRequest(request);
      }
      return;
    }
    auto request = std::make_shared<SendMessageRequest>();
    request->chatId = fields[1];
    request->chatMessage.text = fields[3];
    request->onResult = [result](bool success) { Write(result, success ? "accepted\n" : "failed\n"); };
    protocol->second->SendRequest(request);
  }

  std::unordered_map<std::string, std::shared_ptr<Protocol>> m_Protocols;
  std::string m_Dir, m_Nonce;
  int m_Lock = -1;
  std::atomic<bool> m_Stop { false };
  std::thread m_Thread;
};
