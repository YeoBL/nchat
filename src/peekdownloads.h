// Narrow local IPC: download an attachment already identified in nchat's cache.
// No UI selection, history fetching, open-chat, or mark-read operations.
#pragma once
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include "fileutil.h"
#include "messagecache.h"
#include "protocolutil.h"
#include "strutil.h"

class PeekDownloads
{
public:
  explicit PeekDownloads(const std::unordered_map<std::string, std::shared_ptr<Protocol>>& protocols)
    : m_Protocols(protocols), m_Dir(FileUtil::GetApplicationDir() + "/peek-downloads")
  {
    mkdir(m_Dir.c_str(), 0700);
    struct stat st;
    if (lstat(m_Dir.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return;
    const std::string marker = m_Dir + "/server";
    int fd = open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    const std::string pid = std::to_string(getpid());
    write(fd, pid.data(), pid.size());
    close(fd);
    m_Thread = std::thread([this]()
    {
      while (!m_Stop)
      {
        for (const auto& name : FileUtil::ListDirNames(m_Dir))
        {
          if (m_Stop) break;
          if (name.size() != 40 || name.substr(32) != ".request") continue;
          Process(m_Dir + "/" + name);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    });
  }

  ~PeekDownloads()
  {
    m_Stop = true;
    if (m_Thread.joinable())
    {
      m_Thread.join();
      unlink((m_Dir + "/server").c_str());
    }
  }

private:
  void Process(const std::string& path)
  {
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077) || st.st_size > 8192)
    {
      close(fd);
      return;
    }
    char data[8193];
    ssize_t len = read(fd, data, sizeof(data)-1);
    close(fd);
    unlink(path.c_str());
    if (len <= 0) return;
    std::istringstream input(std::string(data, len));
    std::string fields[3];
    for (auto& field : fields)
    {
      if (!std::getline(input, field) || field.empty() || (field.size() % 2) ||
          field.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return;
      field = StrUtil::StrFromHex(field);
    }
    auto protocol = m_Protocols.find(fields[0]);
    if (protocol == m_Protocols.end()) return;
    std::vector<ChatMessage> messages;
    // Synchronous cache lookup only; it does not fetch or display the chat.
    if (!MessageCache::GetOneMessage(fields[0], fields[1], fields[2], messages) || messages.empty()) return;
    if (ProtocolUtil::IsViewOnce(messages.front())) return;
    const FileInfo file = ProtocolUtil::FileInfoFromHex(messages.front().fileInfo);
    if (file.fileId.empty()) return;
    auto request = std::make_shared<DownloadFileRequest>();
    request->chatId = fields[1];
    request->msgId = fields[2];
    request->fileId = file.fileId;
    request->downloadFileAction = DownloadFileActionCacheOnly;
    protocol->second->SendRequest(request);
  }

  std::unordered_map<std::string, std::shared_ptr<Protocol>> m_Protocols;
  std::string m_Dir;
  std::atomic<bool> m_Stop { false };
  std::thread m_Thread;
};
