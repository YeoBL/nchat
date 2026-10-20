#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "clipboard.h"
#include "protocol.h"
#include "strutil.h"
#include "uicolorconfig.h"
#include "uicontroller.h"
#include "uikeyconfig.h"
#include "uilistdialog.h"
#include "uimodel.h"

// Account tools share the existing list navigation and filtering. This helper
// only presents labels; protocol operation names are never shown to the user.
namespace ClientActionUi
{
inline std::string DisplayText(std::string text, bool keepNewlines = false)
{
  for (char& c : text)
    if ((static_cast<unsigned char>(c) < 32 && !(keepNewlines && c == '\n')) || c == 127) c = ' ';
  return text;
}

inline bool Matches(const ClientActionItem& item, const std::string& filter)
{
  return filter.empty() || StrUtil::ToLower(item.title + " " + item.detail).find(filter) != std::string::npos;
}

inline std::map<std::string, std::string> MergeParams(std::map<std::string, std::string> context,
  const std::map<std::string, std::string>& selected)
{
  for (const auto& param : selected) context[param.first] = param.second;
  return context;
}

struct FieldLayout
{
  std::vector<std::wstring> lines = {L""};
  std::vector<int> starts = {0};
  int row = 0, col = 0;
};

inline FieldLayout LayoutField(const std::wstring& text, int width, int pos)
{
  FieldLayout layout;
  width = std::max(1, width);
  int cells = 0;
  for (size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] == L'\n')
    {
      layout.lines.push_back(L"");
      layout.starts.push_back(static_cast<int>(i + 1));
      cells = 0;
      continue;
    }
    const int charCells = std::max(0, wcwidth(text[i]));
    if (cells + charCells > width && !layout.lines.back().empty())
    {
      layout.lines.push_back(L"");
      layout.starts.push_back(static_cast<int>(i));
      cells = 0;
    }
    layout.lines.back() += text[i];
    cells += charCells;
  }
  pos = std::max(0, std::min(static_cast<int>(text.size()), pos));
  for (size_t i = 1; i < layout.starts.size(); ++i)
    if (layout.starts[i] <= pos) layout.row = static_cast<int>(i);
  layout.col = StrUtil::WStringWidth(layout.lines[layout.row].substr(0, pos - layout.starts[layout.row]));
  return layout;
}

inline int MoveFieldLine(const std::wstring& text, int width, int pos, bool down)
{
  const auto layout = LayoutField(text, width, pos);
  const int row = layout.row + (down ? 1 : -1);
  if (row < 0) return 0;
  if (row >= static_cast<int>(layout.lines.size())) return static_cast<int>(text.size());
  int offset = 0, cells = 0;
  for (const auto c : layout.lines[row])
  {
    cells += std::max(0, wcwidth(c));
    if (cells > layout.col) break;
    ++offset;
  }
  return layout.starts[row] + offset;
}

// Field values can contain newlines (messages, descriptions and poll options).
// Enter saves only this field; the controller still requires an explicit Submit.
class FieldDialog : public UiDialog
{
public:
  FieldDialog(const UiDialogParams& params, const ClientActionField& field)
    : UiDialog(params), m_Label(DisplayText(field.label)), m_Entry(StrUtil::ToWString(DisplayText(field.value, true))),
      m_Pos(static_cast<int>(m_Entry.size()))
  {
    m_Model->SetMessageDialogActive(true);
  }

  ~FieldDialog() override { m_Model->SetMessageDialogActive(false); }

  bool Run()
  {
    m_Model->Draw();
    Draw();
    while (m_Running)
    {
      const wint_t key = UiController::GetKey(50);
      if (key) { HandleKey(key); Draw(); }
    }
    return m_Accepted;
  }

  std::string Value() const { return StrUtil::ToString(m_Entry); }

private:
  void HandleKey(wint_t key)
  {
    if (key == UiKeyConfig::GetKey("cancel") || key == UiKeyConfig::GetKey("quit"))
      m_Running = false;
    else if (key == 14) m_Entry.insert(m_Pos++, 1, L'\n'); // Ctrl-N also works when Enter is configured as LF.
    else if (key == UiKeyConfig::GetKey("paste"))
    {
      const auto text = StrUtil::ToWString(DisplayText(Clipboard::GetText(), true));
      m_Entry.insert(m_Pos, text);
      m_Pos += static_cast<int>(text.size());
    }
    else if (key == UiKeyConfig::GetKey("ok"))
    {
      m_Accepted = true;
      m_Running = false;
    }
    else if (key == UiKeyConfig::GetKey("terminal_resize"))
    {
      Cleanup();
      m_Model->ReinitView();
      m_Model->Draw();
      Init();
    }
    else if (key == UiKeyConfig::GetKey("terminal_focus_in")) m_Model->SetTerminalActive(true);
    else if (key == UiKeyConfig::GetKey("terminal_focus_out")) m_Model->SetTerminalActive(false);
    else if (key == UiKeyConfig::GetKey("left")) m_Pos = std::max(0, m_Pos - 1);
    else if (key == UiKeyConfig::GetKey("right")) m_Pos = std::min(static_cast<int>(m_Entry.size()), m_Pos + 1);
    else if (key == UiKeyConfig::GetKey("home")) m_Pos = 0;
    else if (key == UiKeyConfig::GetKey("end")) m_Pos = static_cast<int>(m_Entry.size());
    else if (key == UiKeyConfig::GetKey("begin_line")) StrUtil::JumpToPrevMatch(m_Entry, m_Pos, -1, L"\n");
    else if (key == UiKeyConfig::GetKey("end_line")) StrUtil::JumpToNextMatch(m_Entry, m_Pos, 0, L"\n");
    else if (key == UiKeyConfig::GetKey("delete_line_before_cursor"))
      StrUtil::DeleteToPrevMatch(m_Entry, m_Pos, -1, L"\n");
    else if (key == UiKeyConfig::GetKey("delete_line_after_cursor"))
      StrUtil::DeleteToNextMatch(m_Entry, m_Pos, 0, L"\n");
    else if (key == UiKeyConfig::GetKey("backspace") || key == UiKeyConfig::GetKey("backspace_alt"))
    {
      if (m_Pos > 0) m_Entry.erase(--m_Pos, 1);
    }
    else if (key == UiKeyConfig::GetKey("delete"))
    {
      if (m_Pos < static_cast<int>(m_Entry.size())) m_Entry.erase(m_Pos, 1);
    }
    else if (key == UiKeyConfig::GetKey("up") || key == UiKeyConfig::GetKey("down"))
      m_Pos = MoveFieldLine(m_Entry, std::max(1, m_W - 1), m_Pos, key == UiKeyConfig::GetKey("down"));
    else if (StrUtil::IsValidTextKey(key)) m_Entry.insert(m_Pos++, 1, key);
  }

  void Draw()
  {
    if (!m_Running) return;
    const int color = UiColorConfig::GetColorPair("dialog_color");
    const int attr = UiColorConfig::GetAttribute("dialog_attr");
    werase(m_Win);
    wbkgd(m_Win, color | ' ');
    wattron(m_Win, attr | color);
    auto labels = StrUtil::WordWrap(StrUtil::ToWString(m_Label), std::max(1, m_W), false, false, false, 2);
    const int labelRows = std::min(std::max(1, m_H / 3), static_cast<int>(labels.size()));
    for (int i = 0; i < labelRows; ++i) mvwaddnwstr(m_Win, i, 0, labels[i].c_str(), m_W);
    const int start = std::min(m_H - 1, labelRows + 1);
    const int height = std::max(1, m_H - start);
    const auto layout = LayoutField(m_Entry, std::max(1, m_W - 1), m_Pos);
    const int offset = std::max(0, layout.row - height + 1);
    for (int i = offset; i < std::min(static_cast<int>(layout.lines.size()), offset + height); ++i)
      mvwaddnwstr(m_Win, start + i - offset, 0, layout.lines[i].c_str(), m_W);
    wmove(m_Win, std::min(m_H - 1, start + layout.row - offset),
      std::min(std::max(0, m_W - 1), layout.col));
    wattroff(m_Win, attr | color);
    SetFooter("Enter: save | Ctrl-N: new line | Alt-V: paste | Ctrl-C: cancel");
    wrefresh(m_Win);
    curs_set(1);
  }

  std::string m_Label;
  std::wstring m_Entry;
  int m_Pos = 0;
  bool m_Running = true, m_Accepted = false;
};
}

class UiClientActionDialog : public UiListDialog
{
public:
  enum Choice { Close, Back, Item, Preview };
  using Dispatcher = std::function<bool(const std::string&, const std::map<std::string, std::string>&,
    std::function<void(ClientActionResult)>)>;
  using OpenAction = std::function<bool(const ClientActionItem&)>;

  struct Page
  {
    ClientActionResult result;
    std::map<std::string, std::string> context;
    std::wstring filter;
    int index = 0;
  };

  UiClientActionDialog(const UiDialogParams& params, const Page& page, bool canBack,
    const std::shared_ptr<Protocol>& protocol = nullptr,
    const std::shared_ptr<ClientActionRequest>& request = nullptr, const Dispatcher& dispatch = {})
    : UiListDialog(params, false), m_Page(page), m_CanBack(canBack)
  {
    m_FilterStr = page.filter;
    m_Index = page.index;
    if (request && protocol)
    {
      request->params.erase("_replacePage");
      m_Page.context = request->params;
      std::weak_ptr<State> weak = m_State;
      request->onResult = [weak](ClientActionResult result)
      {
        if (auto state = weak.lock())
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (!state->ready) { state->result = std::move(result); state->ready = true; }
        }
      };
      m_Pending = true;
      if (!dispatch || !dispatch(request->operation, request->params, request->onResult))
        protocol->SendRequest(request);
    }
    UpdateList();
  }

  Choice SelectedChoice() const { return m_Choice; }
  ClientActionItem SelectedItem() const { return m_Selected; }
  Page CurrentPage() const
  {
    Page page = m_Page;
    page.filter = m_FilterStr;
    page.index = m_Index;
    return page;
  }

  static void RunActions(UiModel* model, const std::shared_ptr<Protocol>& protocol,
    const std::string& title, const std::string& operation,
    const std::map<std::string, std::string>& params = {},
    const std::function<void(const std::string&)>& openFile = {},
    const Dispatcher& dispatch = {}, const OpenAction& openAction = {})
  {
    if (!model || !protocol) return;
    std::vector<Page> history;
    Page page;
    auto request = std::make_shared<ClientActionRequest>();
    request->operation = operation;
    request->params = params;
    while (true)
    {
      Choice choice;
      ClientActionItem item;
      {
        UiDialogParams dialogParams(model, title, 0.85, 0.8);
        UiClientActionDialog dialog(dialogParams, page, !history.empty(), protocol, request, dispatch);
        dialog.Run();
        choice = dialog.SelectedChoice();
        item = dialog.SelectedItem();
        page = dialog.CurrentPage();
      }
      request.reset();
      if (choice == Close) break;
      if (choice == Back)
      {
        if (history.empty()) break;
        page = history.back();
        history.pop_back();
        continue;
      }
      if (choice == Preview)
      {
        if (openFile) openFile(page.result.filePath);
        continue;
      }
      if (item.operation.empty()) continue;
      const bool replacePage = item.params.count("_replacePage") && item.params.at("_replacePage") == "1";
      item.params.erase("_replacePage");
      item.params = ClientActionUi::MergeParams(page.context, item.params);
      if (!PrepareAction(model, item)) continue;
      if (openAction && openAction(item)) break;
      if (!replacePage) history.push_back(page);
      page = Page();
      request = std::make_shared<ClientActionRequest>();
      request->operation = item.operation;
      request->params = item.params;
    }
    model->ReinitView();
  }

protected:
  void OnSelect() override
  {
    if (m_Index < 0 || m_Index >= static_cast<int>(m_Visible.size())) return;
    const int selected = m_Visible[m_Index];
    if (selected == -4)
    {
      m_Started = std::chrono::steady_clock::now();
      m_TimedOut = false;
      UpdateList();
      return;
    }
    if (selected == -3) m_Choice = Back;
    else if (selected == -2) m_Choice = Preview;
    else if (selected >= 0)
    {
      m_Choice = Item;
      m_Selected = m_Page.result.items[selected];
    }
    else return;
    m_Running = false;
    m_Result = true;
  }

  void OnBack() override
  {
    m_Choice = m_CanBack ? Back : Close;
    m_Running = false;
  }

  bool OnTimer() override
  {
    if (!m_Pending) return false;
    bool ready;
    {
      std::lock_guard<std::mutex> lock(m_State->mutex);
      ready = m_State->ready;
      if (ready) m_Page.result = std::move(m_State->result);
    }
    if (ready)
    {
      m_Pending = false;
      m_FilterStr.clear();
      m_Index = 0;
      UpdateList();
      UpdateFooter();
      return true;
    }
    if (!m_TimedOut && std::chrono::steady_clock::now() - m_Started > std::chrono::seconds(30))
    {
      m_TimedOut = true;
      UpdateList();
      return true;
    }
    return false;
  }

  void UpdateList() override
  {
    m_Items.clear();
    m_Visible.clear();
    auto add = [&](const std::string& text, int item)
    {
      auto lines = StrUtil::WordWrap(StrUtil::ToWString(ClientActionUi::DisplayText(text, true)),
        std::max(1, m_W), false, false, false, 2);
      for (const auto& line : lines)
      {
        m_Items.push_back(StrUtil::TrimPadWString(line, m_W));
        m_Visible.push_back(item);
      }
    };
    if (m_CanBack) add("‹ Back", -3);
    if (m_Pending)
    {
      add(m_TimedOut ? "Still waiting for the service. The request may still complete." : "Loading…", -1);
      if (m_TimedOut) add("Keep waiting", -4);
      add("Ctrl-C closes this menu.", -1);
    }
    else
    {
      if (!m_Page.result.title.empty()) add(m_Page.result.title, -1);
      if (!m_Page.result.success) add("Couldn't complete this action.", -1);
      if (!m_Page.result.text.empty()) add(m_Page.result.text, -1);
      if (!m_Page.result.filePath.empty()) add("Open preview", -2);
      const auto filter = StrUtil::ToLower(StrUtil::ToString(m_FilterStr));
      int matches = 0;
      for (size_t i = 0; i < m_Page.result.items.size(); ++i)
      {
        const auto& item = m_Page.result.items[i];
        if (!ClientActionUi::Matches(item, filter)) continue;
        ++matches;
        add("› " + item.title, static_cast<int>(i));
        if (!item.detail.empty()) add("  " + item.detail, static_cast<int>(i));
      }
      if (!filter.empty() && matches == 0) add("No matching items. Backspace clears the search.", -1);
      else if (m_Page.result.items.empty() && m_Page.result.text.empty() && m_Page.result.filePath.empty())
        add("No items available.", -1);
    }
    m_Index = std::max(0, std::min(m_Index, std::max(0, static_cast<int>(m_Items.size()) - 1)));
  }

private:
  static bool PrepareAction(UiModel* model, ClientActionItem& item)
  {
    if (item.fields.empty() && !item.confirm) return true;
    bool edit = !item.fields.empty();
    while (true)
    {
      if (edit)
      {
        for (size_t i = 0; i < item.fields.size(); ++i)
        {
          auto& field = item.fields[i];
          UiDialogParams params(model, ClientActionUi::DisplayText(item.title) + " · " +
            std::to_string(i + 1) + "/" + std::to_string(item.fields.size()), 0.8, 0.55);
          ClientActionUi::FieldDialog dialog(params, field);
          if (!dialog.Run()) return false;
          field.value = dialog.Value();
          item.params[field.key] = field.value;
        }
      }
      Page review;
      review.result.title = "Review: " + item.title;
      review.result.text = item.detail;
      for (const auto& field : item.fields)
        review.result.text += "\n\n" + field.label + "\n" + field.value;
      ClientActionItem cancel, submit;
      cancel.title = "Cancel";
      cancel.operation = "cancel";
      submit.title = "Confirm: " + item.title;
      submit.operation = "submit";
      review.result.items = {cancel, submit};
      if (!item.fields.empty())
      {
        ClientActionItem change;
        change.title = "Edit details";
        change.operation = "edit";
        review.result.items.push_back(change);
      }
      ClientActionItem selected;
      Choice choice;
      {
        UiDialogParams params(model, "Confirm action", 0.85, 0.8);
        UiClientActionDialog dialog(params, review, false);
        dialog.Run();
        choice = dialog.SelectedChoice();
        selected = dialog.SelectedItem();
      }
      if (choice != Item || selected.operation == "cancel") return false;
      if (selected.operation == "submit") return true;
      edit = true;
    }
  }

  struct State
  {
    std::mutex mutex;
    bool ready = false;
    ClientActionResult result;
  };
  std::shared_ptr<State> m_State = std::make_shared<State>();
  Page m_Page;
  bool m_CanBack = false, m_Pending = false, m_TimedOut = false;
  Choice m_Choice = Close;
  ClientActionItem m_Selected;
  std::vector<int> m_Visible;
  std::chrono::steady_clock::time_point m_Started = std::chrono::steady_clock::now();
};
