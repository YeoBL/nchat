// uihistoryview.cpp
//
// Copyright (c) 2019-2025 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "uihistoryview.h"

#include <chrono>
#include <map>

#include "appconfig.h"
#include "apputil.h"
#include "fileutil.h"
#include "log.h"
#include "protocolutil.h"
#include "strutil.h"
#include "sysutil.h"
#include "timeutil.h"
#include "uicolorconfig.h"
#include "uiconfig.h"
#include "uimodel.h"

namespace
{
  constexpr int s_DefaultThumbnailRows = 18;
  constexpr int64_t s_ThumbnailDebounceMs = 500;
  // All thumbnail output runs on the UI thread, after curses finishes drawing.
  std::string s_PendingThumbnailCommand;
  std::chrono::steady_clock::time_point s_ThumbnailDue;
  unsigned s_ThumbnailDialogDepth = 0;
  uint64_t s_ThumbnailRevision = 0;
  const std::wstring s_ThumbnailLine = L"\x1";

  std::string ThumbnailCommand()
  {
    const std::string configured = UiConfig::GetStr("thumbnail_command");
    return configured.empty() ? "nchat-thumbnails" : configured;
  }

  struct ThumbnailPlacement
  {
    std::string path;
    int row = 0;
    int width = 0;
    int rows = 0;
  };

  bool IsImageAttachment(const FileInfo& p_FileInfo)
  {
    std::string type = p_FileInfo.fileType;
    if (type.empty() && FileUtil::Exists(p_FileInfo.filePath))
    {
      type = FileUtil::GetMimeType(p_FileInfo.filePath);
    }
    if (type.rfind("image/", 0) == 0) return true;

    const std::string name = StrUtil::ToLower(FileUtil::BaseName(p_FileInfo.filePath));
    if ((name == "[photo]") || (name == "[sticker]")) return true;

    static const std::vector<std::string> extensions =
      { ".avif", ".bmp", ".gif", ".heic", ".jpeg", ".jpg", ".png", ".tif", ".tiff", ".webp" };
    for (const auto& extension : extensions)
    {
      if ((name.size() >= extension.size()) &&
          (name.compare(name.size() - extension.size(), extension.size(), extension) == 0)) return true;
    }
    return false;
  }

  int GetThumbnailRows(const std::string& p_Path)
  {
    static std::map<std::string, int> cache;
    auto found = cache.find(p_Path);
    if (found != cache.end()) return found->second;

    std::string output;
    const std::string command = ThumbnailCommand() + " --rows '" +
      StrUtil::EscapeSingleQuote(p_Path) + "'";
    SysUtil::RunCommand(command, &output);
    const int reportedRows = StrUtil::ToInteger(output);
    const int rows = ((reportedRows >= 8) && (reportedRows <= 28)) ?
      reportedRows : s_DefaultThumbnailRows;
    return cache.emplace(p_Path, rows).first->second;
  }

}

UiHistoryView::UiHistoryView(const UiViewParams& p_Params)
  : UiViewBase(p_Params)
{
  if (m_Enabled)
  {
    int hpad = (m_X == 0) ? 0 : 1;
    int vpad = 1;
    int paddedY = m_Y + vpad;
    int paddedX = m_X + hpad;
    m_PaddedH = m_H - (vpad * 2);
    m_PaddedW = m_W - (hpad * 2);
    m_PaddedWin = newwin(m_PaddedH, m_PaddedW, paddedY, paddedX);

    static int attributeTextNormal = UiColorConfig::GetAttribute("history_text_attr");
    static int colorPairTextRecv = UiColorConfig::GetColorPair("history_text_recv_color");
    werase(m_Win);
    wbkgd(m_Win, attributeTextNormal | colorPairTextRecv | ' ');
    wrefresh(m_Win);
  }
}

UiHistoryView::~UiHistoryView()
{
  s_PendingThumbnailCommand.clear();
  if (m_ThumbnailsVisible)
  {
    SysUtil::RunCommand(ThumbnailCommand());
  }

  if (m_PaddedWin != nullptr)
  {
    delwin(m_PaddedWin);
    m_PaddedWin = nullptr;
  }
}

void UiHistoryView::Draw()
{
  if (m_ThumbnailRevision != s_ThumbnailRevision)
  {
    m_ThumbnailRevision = s_ThumbnailRevision;
    m_ThumbnailCommand.clear();
    m_Dirty = true;
  }
  if (!m_Enabled || !m_Dirty) return;
  m_Dirty = false;

  curs_set(0);

  static int colorPairTextSent = UiColorConfig::GetColorPair("history_text_sent_color");
  static int colorPairTextRecv = UiColorConfig::GetColorPair("history_text_recv_color");
  static int colorPairTextQuoted = UiColorConfig::GetColorPair("history_text_quoted_color");
  static int colorPairTextReaction = UiColorConfig::GetColorPair("history_text_reaction_color");
  static int colorPairTextAttachment = UiColorConfig::GetColorPair("history_text_attachment_color");
  static int attributeTextNormal = UiColorConfig::GetAttribute("history_text_attr");
  static int attributeTextSelected = UiColorConfig::GetAttribute("history_text_attr_selected");

  static int colorPairNameSent = UiColorConfig::GetColorPair("history_name_sent_color");
  static int colorPairNameRecv = UiColorConfig::GetColorPair("history_name_recv_color");
  static int attributeNameNormal = UiColorConfig::GetAttribute("history_name_attr");
  static int attributeNameSelected = UiColorConfig::GetAttribute("history_name_attr_selected");

  static std::wstring attachmentIndicator =
    StrUtil::ToWString(UiConfig::GetStr("attachment_indicator") + " ");
  static std::wstring quoteIndicator = L"> ";

  std::pair<std::string, std::string>& currentChat = m_Model->GetCurrentChatLocked();
  const bool emojiEnabled = m_Model->GetEmojiEnabledLocked();
  static const bool developerMode = AppUtil::GetDeveloperMode();

  std::vector<std::string>& messageVec =
    m_Model->GetMessageVecLocked(currentChat.first, currentChat.second);
  std::unordered_map<std::string, ChatMessage>& messages =
    m_Model->GetMessagesLocked(currentChat.first, currentChat.second);
  int messageOffset = std::max(m_Model->GetMessageOffsetLocked(currentChat.first, currentChat.second), 0);

  werase(m_PaddedWin);
  wbkgd(m_PaddedWin, attributeTextNormal | colorPairTextRecv | ' ');

  m_HistoryShowCount = 0;
  std::vector<ThumbnailPlacement> thumbnailPlacements;

  bool firstMessage = true;
  int y = m_PaddedH - 1;
  for (auto it = std::next(messageVec.begin(), messageOffset); it != messageVec.end(); ++it)
  {
    bool isSelectedMessage = firstMessage && m_Model->GetSelectMessageActiveLocked();

    auto msgIt = messages.find(*it);
    if (msgIt == messages.end())
    {
      LOG_WARNING("message %s missing", it->c_str());
      continue;
    }

    ChatMessage& msg = msgIt->second;

    int attributeText = isSelectedMessage ? attributeTextSelected : attributeTextNormal;
    int colorPairText = [&]()
    {
      if (msg.isOutgoing) return colorPairTextSent;

      if (msg.senderId == currentChat.second) return colorPairTextRecv;

      static bool isUserColor = UiColorConfig::IsUserColor("history_text_recv_group_color");
      if (!isUserColor)
      {
        static int colorPairGroup = UiColorConfig::GetColorPair("history_text_recv_group_color");
        return colorPairGroup;
      }

      int colorPairGroup = UiColorConfig::GetUserColorPair("history_text_recv_group_color",
                                                           msg.senderId);
      return colorPairGroup;
    }();

    std::vector<std::wstring> wlines;
    std::string thumbnailPath;
    int thumbnailWidth = 0;
    int thumbnailRows = 0;
    if (!msg.text.empty())
    {
      std::string text = msg.text;
      StrUtil::SanitizeMessageStr(text);
      if (!emojiEnabled)
      {
        text = StrUtil::Textize(text);
      }

      wlines = StrUtil::WordWrap(StrUtil::ToWString(text), m_PaddedW, false, false, false, 2);
    }

    // Quoted message
    if (!msg.quotedId.empty())
    {
      std::string quotedText;
      auto quotedIt = messages.find(msg.quotedId);
      if (quotedIt != messages.end())
      {
        if (!quotedIt->second.text.empty())
        {
          quotedText = StrUtil::Split(quotedIt->second.text, '\n').at(0);
          if (!emojiEnabled)
          {
            quotedText = StrUtil::Textize(quotedText);
          }
        }
        else if (!quotedIt->second.fileInfo.empty())
        {
          FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(quotedIt->second.fileInfo);
          quotedText = FileUtil::BaseName(fileInfo.filePath);
        }
      }
      else
      {
        m_Model->FetchCachedMessageLocked(currentChat.first, currentChat.second, msg.quotedId);
        quotedText = "";
      }

      int maxQuoteLen = m_PaddedW - 3;
      std::wstring quote = quoteIndicator + StrUtil::ToWString(quotedText);
      if (StrUtil::WStringWidth(quote) > maxQuoteLen)
      {
        quote = StrUtil::TrimPadWString(quote, maxQuoteLen) + L"...";
      }

      wlines.insert(wlines.begin(), quote);
    }

    // File attachment
    if (!msg.fileInfo.empty())
    {
      FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(msg.fileInfo);

      // special case handling selection-triggered download, and handling cache's old setting
      static const bool isAttachmentPrefetchAll =
        (AppConfig::GetNum("attachment_prefetch") == AttachmentPrefetchAll);
      static const bool isAttachmentPrefetchSelected =
        (AppConfig::GetNum("attachment_prefetch") == AttachmentPrefetchSelected);
      if (isAttachmentPrefetchAll || (isSelectedMessage && isAttachmentPrefetchSelected) ||
          IsImageAttachment(fileInfo))
      {
        if (!UiModel::IsAttachmentDownloaded(fileInfo) && UiModel::IsAttachmentDownloadable(fileInfo))
        {
          m_Model->DownloadAttachmentLocked(currentChat.first, currentChat.second, *it,
                                            fileInfo.fileId, DownloadFileActionNone);
          fileInfo = ProtocolUtil::FileInfoFromHex(msg.fileInfo);
        }
      }

      std::string fileName = FileUtil::BaseName(fileInfo.filePath);
      std::string fileStatus;
      if (fileInfo.fileStatus == FileStatusNone)
      {
        // should not happen
        static const std::string statusNone = " -";
        fileStatus = statusNone;
      }
      else if (fileInfo.fileStatus == FileStatusNotDownloaded)
      {
        static const std::string statusNotDownloaded = " " + UiConfig::GetStr("downloadable_indicator");
        fileStatus = statusNotDownloaded;
      }
      else if (fileInfo.fileStatus == FileStatusDownloaded)
      {
        static const std::string statusDownloaded = "";
        fileStatus = statusDownloaded;
      }
      else if (fileInfo.fileStatus == FileStatusDownloading)
      {
        static const std::string statusDownloading = " " + UiConfig::GetStr("syncing_indicator");
        fileStatus = statusDownloading;
      }
      else if (fileInfo.fileStatus == FileStatusDownloadFailed)
      {
        static const std::string statusDownloadFailed = " " + UiConfig::GetStr("failed_indicator");
        fileStatus = statusDownloadFailed;
      }

      std::wstring fileStr = attachmentIndicator + StrUtil::ToWString(fileName + fileStatus);
      wlines.insert(wlines.begin(), fileStr);

      if ((fileInfo.fileStatus == FileStatusDownloaded) &&
          FileUtil::Exists(fileInfo.filePath) && IsImageAttachment(fileInfo))
      {
        thumbnailPath = fileInfo.filePath;
        thumbnailWidth = std::min(42, std::max(12, m_PaddedW / 3));
        thumbnailRows = GetThumbnailRows(thumbnailPath);
        wlines.insert(std::next(wlines.begin()), thumbnailRows, s_ThumbnailLine);
      }
    }

    // Reactions
    int reactionLines = 0;
    static bool reactionsEnabled = UiConfig::GetBool("reactions_enabled");
    if (reactionsEnabled)
    {
      std::string selfEmoji;
      auto sit = msg.reactions.senderEmojis.find(s_ReactionsSelfId);
      if (sit != msg.reactions.senderEmojis.end())
      {
        selfEmoji = sit->second;
      }

      // Allow also if we have self emoji, even if not yet consolidated into count
      if (!msg.reactions.emojiCounts.empty() || !selfEmoji.empty())
      {
        bool foundSelf = false;
        std::string reactionsText;
        std::multimap<float, std::string> emojiCountsSorted;
        for (const auto& emojiCount : msg.reactions.emojiCounts)
        {
          float count = emojiCount.second;
          if (emojiCount.first == selfEmoji)
          {
            count += 0.1; // for equal count, prioritize own selected reaction
            foundSelf = true;
          }

          emojiCountsSorted.insert(std::make_pair(count, emojiCount.first));
        }

        if (!foundSelf && !selfEmoji.empty())
        {
          LOG_DEBUG("insert missing reaction for self");
          emojiCountsSorted.insert(std::make_pair(1.1, selfEmoji));
        }

        bool firstReaction = true;
        for (auto emojiCount = emojiCountsSorted.rbegin(); emojiCount != emojiCountsSorted.rend(); ++emojiCount)
        {
          reactionsText += (firstReaction ? " " : "  ");
          if (emojiCount->second == selfEmoji)
          {
            // Highlight own reaction emoji
            reactionsText += "" + emojiCount->second + "*";
          }
          else
          {
            reactionsText += emojiCount->second;
          }

          if (emojiCount->first > 1.5)
          {
            reactionsText += " " + FileUtil::GetSuffixedCount(static_cast<ssize_t>(emojiCount->first));
          }

          firstReaction = false;
        }

        if (!reactionsText.empty())
        {
          if (!emojiEnabled)
          {
            reactionsText = StrUtil::Textize(reactionsText);
          }

          const int maxReactionsLen = m_PaddedW - 4;
          std::wstring reactions = StrUtil::ToWString(reactionsText);
          if (StrUtil::WStringWidth(reactions) > maxReactionsLen)
          {
            reactions = StrUtil::TrimPadWString(reactions, maxReactionsLen) + L"... ";
          }
          else
          {
            reactions += L" ";
          }

          wlines.insert(wlines.end(), reactions);
          reactionLines = 1;
        }
      }
    }

    const int maxMessageLines = (m_PaddedH - 1);
    if (firstMessage && ((int)wlines.size() > maxMessageLines))
    {
      wlines.resize(maxMessageLines - 1);
      wlines.push_back(L"[...]");
      reactionLines = 0;
    }

    int thumbnailRow = thumbnailPath.empty() ? -1 : thumbnailRows - 1;
    for (auto wline = wlines.rbegin(); wline != wlines.rend(); ++wline)
    {
      bool isThumbnail = (*wline == s_ThumbnailLine);
      bool isAttachment = (wline->rfind(attachmentIndicator, 0) == 0);
      bool isQuote = (wline->rfind(quoteIndicator, 0) == 0);
      bool isReaction = (reactionLines == 1) && (std::distance(wline, wlines.rbegin()) == 0);

      if (isThumbnail)
      {
        if (!thumbnailPath.empty() && (thumbnailRow == 0))
        {
          thumbnailPlacements.push_back({ thumbnailPath, y, thumbnailWidth, thumbnailRows });
        }
        --thumbnailRow;
      }
      else if (isAttachment)
      {
        wattron(m_PaddedWin, attributeText | colorPairTextAttachment);
      }
      else if (isQuote)
      {
        wattron(m_PaddedWin, attributeText | colorPairTextQuoted);
      }
      else if (isReaction)
      {
        wattron(m_PaddedWin, attributeTextNormal | colorPairTextReaction);
      }
      else
      {
        wattron(m_PaddedWin, attributeText | colorPairText);
      }

      if (!isThumbnail)
      {
        // Leave wrapped message text unpadded: terminal URL detection joins adjacent
        // rows, but an inserted trailing space splits a long URL at every wrap.
        const std::wstring wdisp = (isReaction || (!isAttachment && !isQuote)) ?
          *wline : StrUtil::TrimPadWString(*wline, m_PaddedW);
        mvwaddnwstr(m_PaddedWin, y, 0, wdisp.c_str(), std::min((int)wdisp.size(), m_PaddedW));
      }

      if (!isThumbnail && isAttachment)
      {
        wattroff(m_PaddedWin, attributeText | colorPairTextAttachment);
      }
      else if (!isThumbnail && isQuote)
      {
        wattroff(m_PaddedWin, attributeText | colorPairTextQuoted);
      }
      else if (!isThumbnail && isReaction)
      {
        wattroff(m_PaddedWin, attributeTextNormal | colorPairTextReaction);
      }
      else if (!isThumbnail)
      {
        wattroff(m_PaddedWin, attributeText | colorPairText);
      }

      if (--y < 0) break;
    }

    if (y < 0) break;

    int attributeName = isSelectedMessage ? attributeNameSelected : attributeNameNormal;
    int colorPairName = [&]()
    {
      if (msg.isOutgoing) return colorPairNameSent;

      if (msg.senderId == currentChat.second) return colorPairNameRecv;

      static bool isUserColor = UiColorConfig::IsUserColor("history_name_recv_group_color");
      if (!isUserColor)
      {
        static int colorPairGroup = UiColorConfig::GetColorPair("history_name_recv_group_color");
        return colorPairGroup;
      }

      int colorPairGroup = UiColorConfig::GetUserColorPair("history_name_recv_group_color",
                                                           msg.senderId);
      return colorPairGroup;
    }();

    wattron(m_PaddedWin, attributeName | colorPairName);
    std::string name = m_Model->GetContactNameLocked(currentChat.first, msg.senderId);
    if (!emojiEnabled)
    {
      name = StrUtil::Textize(name);
    }

    std::wstring wsender = StrUtil::ToWString(name);
    std::wstring wtime;
    if (developerMode)
    {
      wtime = L" (" + StrUtil::ToWString(std::to_string(msg.timeSent)) + L")";
    }
    else
    {
      if (msg.timeSent != std::numeric_limits<int64_t>::max())
      {
        wtime = L" (" + StrUtil::ToWString(TimeUtil::GetTimeString(msg.timeSent, false /* p_IsExport */)) + L")";
      }
    }

    m_Model->MarkReadLocked(currentChat.first, currentChat.second, *it, (!msg.isOutgoing && !msg.isRead));

    static const std::string readIndicator = " " + UiConfig::GetStr("read_indicator");
    static const std::string editedIndicator = " " + UiConfig::GetStr("edited_indicator");
    static const std::string pinnedIndicator = " " + UiConfig::GetStr("pinned_indicator");
    std::wstring wreceipt = StrUtil::ToWString(msg.isRead ? readIndicator : "");
    std::wstring wedited = StrUtil::ToWString(msg.isEdited ? editedIndicator : "");
    std::wstring wpinned = StrUtil::ToWString(msg.isPinned ? pinnedIndicator : "");
    std::wstring wheader = wsender + wtime + wreceipt + wedited + wpinned;

    if (developerMode)
    {
      wheader = wheader +
        L" msg " + StrUtil::ToWString(msg.id) +
        L" user " + StrUtil::ToWString(msg.senderId);
    }

    std::wstring wdisp = StrUtil::TrimPadWString(wheader, m_PaddedW);
    mvwaddnwstr(m_PaddedWin, y, 0, wdisp.c_str(), std::min((int)wdisp.size(), m_PaddedW));

    wattroff(m_PaddedWin, attributeName | colorPairName);

    ++m_HistoryShowCount;

    if (--y < 0) break;

    if (--y < 0) break;

    firstMessage = false;
  }

  wrefresh(m_PaddedWin);

  if (s_ThumbnailDialogDepth != 0)
  {
    m_ThumbnailCommand.clear();
    return;
  }

  std::string thumbnailCommand = ThumbnailCommand();
  for (const auto& placement : thumbnailPlacements)
  {
    thumbnailCommand += " '" + StrUtil::EscapeSingleQuote(placement.path) + "' " +
      std::to_string(m_X + 1) + " " + std::to_string(m_Y + placement.row + 1) + " " +
      std::to_string(placement.width) + " " + std::to_string(placement.rows);
  }
  if (thumbnailCommand != m_ThumbnailCommand)
  {
    if (m_ThumbnailsVisible)
    {
      SysUtil::RunCommand(ThumbnailCommand());
      m_ThumbnailsVisible = false;
    }

    m_ThumbnailCommand = thumbnailCommand;
    m_ThumbnailCommandHasImages = !thumbnailPlacements.empty();
    m_ThumbnailsVisible = m_ThumbnailCommandHasImages;

    s_PendingThumbnailCommand = m_ThumbnailCommand;
    s_ThumbnailDue = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(s_ThumbnailDebounceMs);
  }
}

void UiHistoryView::SuspendThumbnails()
{
  ++s_ThumbnailDialogDepth;
  s_PendingThumbnailCommand.clear();
  SysUtil::RunCommand(ThumbnailCommand());
}

void UiHistoryView::ResumeThumbnails()
{
  if (s_ThumbnailDialogDepth != 0) --s_ThumbnailDialogDepth;
  ++s_ThumbnailRevision;
}

void UiHistoryView::FlushThumbnails()
{
  if ((s_ThumbnailDialogDepth == 0) && !s_PendingThumbnailCommand.empty() &&
      (std::chrono::steady_clock::now() >= s_ThumbnailDue))
  {
    const std::string command = std::move(s_PendingThumbnailCommand);
    s_PendingThumbnailCommand.clear();
    SysUtil::RunCommand(command);
  }
}

int UiHistoryView::GetHistoryShowCount()
{
  return m_HistoryShowCount;
}
