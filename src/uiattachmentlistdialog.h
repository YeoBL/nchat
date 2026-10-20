#pragma once

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "fileutil.h"
#include "messagecache.h"
#include "protocolutil.h"
#include "strutil.h"
#include "uilistdialog.h"
#include "uimodel.h"

class UiAttachmentListDialog : public UiListDialog
{
  std::vector<MessageCache::Attachment> m_Attachments;
  std::vector<size_t> m_Visible;
  std::vector<std::string> m_Lines;
  std::vector<std::string> m_Search;
  std::map<std::pair<std::string, std::string>, std::string> m_ChatNames;
  MessageCache::Attachment m_Selected;

public:
  explicit UiAttachmentListDialog(const UiDialogParams& params)
    : UiListDialog(params, false), m_Attachments(MessageCache::GetAttachments())
  {
    for (const auto& attachment : m_Attachments)
    {
      const auto chat = std::make_pair(attachment.profileId, attachment.chatId);
      if (!m_ChatNames.count(chat))
        m_ChatNames[chat] = m_Model->GetContactListName(chat.first, chat.second, true, true);
    }
    for (const auto& attachment : m_Attachments)
    {
      const auto chat = std::make_pair(attachment.profileId, attachment.chatId);
      const FileInfo file = ProtocolUtil::FileInfoFromHex(attachment.fileInfo);
      const std::string filename = FileUtil::BaseName(file.filePath);
      std::string caption = attachment.text.substr(0, attachment.text.find('\n'));
      const std::string label = caption.empty() ? filename : caption + " · " + filename;
      m_Lines.push_back(m_ChatNames[chat] + " · " + label);
      m_Search.push_back(StrUtil::ToLower(m_ChatNames[chat] + " " + filename + " " + attachment.text));
    }
    UpdateList();
  }

  MessageCache::Attachment Selected() const { return m_Selected; }

protected:
  void OnSelect() override
  {
    if (m_Index < 0 || m_Index >= static_cast<int>(m_Visible.size())) return;
    m_Selected = m_Attachments[m_Visible[m_Index]];
    m_Result = true;
    m_Running = false;
  }
  void OnBack() override { m_Running = false; }
  bool OnTimer() override { return false; }
  void UpdateList() override
  {
    m_Items.clear();
    m_Visible.clear();
    const std::string filter = StrUtil::ToLower(StrUtil::ToString(m_FilterStr));
    for (size_t i = 0; i < m_Attachments.size(); ++i)
    {
      if (!filter.empty() && m_Search[i].find(filter) == std::string::npos)
        continue;
      m_Items.push_back(StrUtil::TrimPadWString(StrUtil::ToWString(m_Lines[i]), m_W));
      m_Visible.push_back(i);
    }
    m_Index = std::min(m_Index, std::max(0, static_cast<int>(m_Items.size()) - 1));
  }
};
