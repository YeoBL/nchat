// Synthetic provider/conversion fixtures only. This never creates a TDLib
// client, downloads service media, opens a viewer, or sends an opened receipt.
#include "../lib/tgchat/src/tgchat.cpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

using namespace td::td_api;

namespace
{
enum Kind { Ordinary, Immediately, Timer, AutoDelete, StartedTimer };

object_ptr<td::td_api::message> Photo(Kind kind, bool downloaded, const std::string& path, int64_t id = 10)
{
  auto message = make_object<td::td_api::message>();
  message->id_ = id;
  message->chat_id_ = 1001;
  message->sender_id_ = make_object<messageSenderUser>(1002);
  message->date_ = 1700000000;
  message->can_be_saved_ = true;
  if (kind == Immediately) message->self_destruct_type_ = make_object<messageSelfDestructTypeImmediately>();
  if (kind == Timer || kind == StartedTimer)
    message->self_destruct_type_ = make_object<messageSelfDestructTypeTimer>(15);
  if (kind == StartedTimer) message->self_destruct_in_ = 12;
  if (kind == AutoDelete) message->auto_delete_in_ = 86400;
  auto content = make_object<messagePhoto>();
  content->caption_ = make_object<formattedText>();
  content->caption_->text_ = "Synthetic caption";
  content->photo_ = make_object<photo>();
  auto size = make_object<photoSize>();
  size->width_ = 640; size->height_ = 480;
  size->photo_ = make_object<file>();
  size->photo_->id_ = 77;
  size->photo_->remote_ = make_object<remoteFile>();
  size->photo_->remote_->id_ = "synthetic-remote-photo";
  size->photo_->local_ = make_object<localFile>();
  if (downloaded)
  {
    size->photo_->local_->path_ = path;
    size->photo_->local_->is_downloading_completed_ = true;
  }
  content->photo_->sizes_.push_back(std::move(size));
  message->content_ = std::move(content);
  return message;
}
}

struct TelegramMuteHandlerTest
{
  static void Run(const std::string& root)
  {
    TgChat::Impl impl;
    impl.m_ProfileId = "Telegram_view_once_fixture";
    impl.m_Config.Set("markdown_enabled", "0");
    impl.m_Config.Set("markdown_version", "2");
    const std::string path = root + "/synthetic-photo.jpg";
    { std::ofstream fixture(path); fixture << "Synthetic nonprivate media fixture"; }
    for (bool downloaded : {false, true})
    {
      for (const auto kind : {Ordinary, Immediately, Timer, AutoDelete, StartedTimer})
      {
        auto message = Photo(kind, downloaded, path);
        ChatMessage converted;
        impl.TdMessageConvert(*message, converted);
        const auto file = ProtocolUtil::FileInfoFromHex(converted.fileInfo);
        assert(file.isViewOnce == (kind == Immediately));
        assert(ProtocolUtil::IsViewOnce(converted) == (kind == Immediately));
        assert(converted.text == (kind == Immediately ?
          "[View-once photo]\nSynthetic caption" : "Synthetic caption"));
        assert(converted.id == StrUtil::NumToHex(int64_t(10)));
        assert(converted.senderId == StrUtil::NumToHex(int64_t(1002)));
        assert(file.fileId == "synthetic-remote-photo" && file.fileType == "image/jpeg");
        assert(file.filePath == (downloaded ? path : "[Photo]"));
        assert(file.fileStatus == (downloaded ? FileStatusDownloaded : FileStatusNotDownloaded));
        assert(!impl.m_ClientManager && impl.m_Handlers.empty() && impl.m_RequestsQueue.empty());
      }
    }
    for (const int type : {messageExpiredPhoto::ID, messageExpiredVideo::ID,
         messageExpiredVideoNote::ID, messageExpiredVoiceNote::ID})
    {
      auto message = Photo(Immediately, true, path);
      if (type == messageExpiredPhoto::ID) message->content_ = make_object<messageExpiredPhoto>();
      if (type == messageExpiredVideo::ID) message->content_ = make_object<messageExpiredVideo>();
      if (type == messageExpiredVideoNote::ID) message->content_ = make_object<messageExpiredVideoNote>();
      if (type == messageExpiredVoiceNote::ID) message->content_ = make_object<messageExpiredVoiceNote>();
      ChatMessage converted;
      impl.TdMessageConvert(*message, converted);
      assert(converted.fileInfo.empty() && !ProtocolUtil::IsViewOnce(converted));
      if (type == messageExpiredPhoto::ID) assert(converted.text == "[Expired photo]");
      if (type == messageExpiredVideo::ID) assert(converted.text == "[Expired video]");
    }
    std::cout << "PASS: Telegram immediate photo labels/flags preserve captions and existing pending/downloaded file references; timers and auto-delete remain ordinary\n";
    std::cout << "PASS: expired Telegram media exposes no attachment reference and is not mislabeled view-once\n";

    MessageCache::AddProfile(impl.m_ProfileId, false, 1, false, false);
    const auto chatId = StrUtil::NumToHex(int64_t(1001));
    std::vector<std::shared_ptr<NewMessageFileNotify>> published;
    impl.SetMessageHandler([&](std::shared_ptr<ServiceMessage> message)
    {
      assert(message->GetMessageType() == NewMessageFileNotifyType);
      published.push_back(std::static_pointer_cast<NewMessageFileNotify>(message));
    });
    for (const bool once : {false, true})
    {
      const int64_t id = once ? 12 : 11;
      auto message = Photo(once ? Immediately : Ordinary, false, path, id);
      ChatMessage cached;
      impl.TdMessageConvert(*message, cached);
      MessageCache::AddMessages(impl.m_ProfileId, chatId, "", {cached});
      std::vector<ChatMessage> stored;
      for (int retry = 0; retry < 200; ++retry)
      {
        stored.clear();
        if (MessageCache::GetOneMessage(impl.m_ProfileId, chatId, cached.id, stored)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      assert(stored.size() == 1 && ProtocolUtil::FileInfoFromHex(stored.front().fileInfo).isViewOnce == once);
      for (const auto action : {DownloadFileActionOpen, DownloadFileActionCacheOnly})
      {
        const auto before = published.size();
        impl.DownloadFile(chatId, cached.id, "synthetic-remote-photo", StrUtil::NumToHex(int32_t(77)), action);
        assert(!impl.m_ClientManager && impl.m_Handlers.size() == 1);
        auto downloaded = make_object<file>();
        downloaded->id_ = 77;
        downloaded->local_ = make_object<localFile>();
        downloaded->local_->path_ = path;
        downloaded->local_->is_downloading_completed_ = true;
        impl.ProcessResponse({0, impl.m_Handlers.begin()->first, std::move(downloaded)});
        assert(published.size() == before + 1 && impl.m_Handlers.empty());
        const auto& notification = *published.back();
        const auto file = ProtocolUtil::FileInfoFromHex(notification.fileInfo);
        assert(notification.chatId == chatId && notification.msgId == cached.id &&
          notification.downloadFileAction == action);
        assert(file.isViewOnce == once && file.fileId == "synthetic-remote-photo" &&
          file.filePath == path && file.fileStatus == FileStatusDownloaded);
      }
    }
    assert(!impl.m_ClientManager && impl.m_RequestsQueue.empty());
    std::cout << "PASS: production Telegram download callbacks retain cached view-once policy and exact chat/message/action/file references\n";
  }
};

int main(int argc, char** argv)
{
  assert(argc == 2);
  FileUtil::SetApplicationDir(argv[1]);
  AppConfig::Init();
  MessageCache::Init();
  TelegramMuteHandlerTest::Run(argv[1]);
  MessageCache::Cleanup();
  AppConfig::Cleanup();
}
