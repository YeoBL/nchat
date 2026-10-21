#pragma once
#include <cstdio>
#include "appconfig.h"
#include "../src/uicolorconfig.h"
#include "../src/uimessageinfodialog.h"

class ReceiptTestProtocol : public Protocol
{
public:
  std::shared_ptr<GetMessageReceiptsRequest> request;
  int requestCount = 0;
  std::string GetProfileId() const override { return "receipt-test"; }
  std::string GetProfileDisplayName() const override { return "Test"; }
  bool HasFeature(ProtocolFeature) const override { return false; }
  bool IsGroupChat(const std::string&) const override { return true; }
  std::string GetSelfId() const override { return "self"; }
  bool SetupProfile(const std::string&, std::string&) override { return false; }
  bool LoadProfile(const std::string&, const std::string&) override { return false; }
  bool CloseProfile() override { return true; }
  bool Login() override { return false; }
  bool Logout() override { return true; }
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>&) override {}
  void SendRequest(std::shared_ptr<RequestMessage> value) override
  {
    assert(value->GetMessageType() == GetMessageReceiptsRequestType); // No sends/mark-read.
    request = std::static_pointer_cast<GetMessageReceiptsRequest>(value);
    ++requestCount;
  }
};

class ReceiptTestDialog : public UiMessageInfoDialog
{
public:
  using UiMessageInfoDialog::UiMessageInfoDialog;
  using UiMessageInfoDialog::OnTimer;
  using UiMessageInfoDialog::OnBack;
  using UiMessageInfoDialog::OnSelect;
  using UiMessageInfoDialog::UpdateList;
  using UiListDialog::m_Items;
  using UiListDialog::m_FilterStr;
  using UiListDialog::m_Index;
  using UiListDialog::m_Running;
  bool contains(const std::wstring& text) const
  {
    for (const auto& item : m_Items) if (item.find(text) != std::wstring::npos) return true;
    return false;
  }
  void resize() { Cleanup(); Init(); UpdateList(); }
};

static void TestMessageInfoDialog()
{
  FILE* output = tmpfile(); FILE* input = tmpfile();
  assert(output && input);
  SCREEN* screen = newterm("xterm-256color", output, input);
  assert(screen);
  set_term(screen);
  AppConfig::Init();
  UiColorConfig::Init();
  auto protocol = std::make_shared<ReceiptTestProtocol>();
  UiModel model;
  auto request = std::make_shared<GetMessageReceiptsRequest>();
  request->chatId = "test-chat"; request->msgId = "test-message";
  for (const std::string profile : {"Telegram_test", "WhatsAppMd_test"})
  {
    UiDialogParams params(&model, "Message Info", 0.85, 0.75);
    ReceiptTestDialog dialog(params, profile, protocol, request);
    const bool telegram = profile == "Telegram_test";
    assert(dialog.contains(L"Loading receipt"));
    assert(protocol->request->chatId == "test-chat" && protocol->request->msgId == "test-message");
    MessageReceiptInfo info;
    for (int i = 0; i < 100; ++i) info.receipts.push_back({"Person" + std::to_string(i), 100, 200});
    info.receipts.push_back({"UnknownTime", 0, -1});
    info.receipts.push_back({"ReadOnly", 0, 200});
    info.note = "Privacy restricted: older history may be incomplete.";
    protocol->request->onResult(info);
    assert(dialog.OnTimer());
    assert(dialog.contains(L"Read by - 102") == telegram);
    assert(dialog.contains(L"Read by - 101") == !telegram);
    assert(dialog.contains(L"Delivered to - 101") == !telegram);
    assert(dialog.contains(L"UnknownTime  -  time unavailable") == telegram);
    assert(dialog.contains(L"delivery time unavailable (read confirmed)") == !telegram);
    assert(!dialog.contains(L"Privacy restricted") && !dialog.contains(L"timezone"));
    assert(dialog.contains(L"Unavailable due to privacy settings.") == telegram);
    assert(dialog.contains(L"Recorded receipts; history may be incomplete.") == !telegram);
    assert(dialog.m_Items.size() == (telegram ? 105 : 206));
    dialog.m_Index = 80;
    dialog.resize();
    assert(dialog.m_Index == 80);
    dialog.m_FilterStr = L"Person99";
    dialog.UpdateList();
    assert(dialog.m_Items.size() == (telegram ? 2 : 3) && dialog.m_Index == (telegram ? 1 : 2));
    dialog.m_FilterStr.clear();
    auto staleRequest = protocol->request;
    dialog.m_Index = static_cast<int>(dialog.m_Items.size()) - 1;
    const int beforeRefresh = protocol->requestCount;
    dialog.OnSelect();
    assert(protocol->requestCount == beforeRefresh + 1 && dialog.m_Running);
    assert(dialog.contains(L"Loading receipt") && dialog.contains(L"Refresh receipts"));
    staleRequest->onResult(info); // An older response cannot overwrite a refresh.
    assert(!dialog.OnTimer() && dialog.contains(L"Loading receipt"));
    protocol->request->onResult({{}, "Privacy restricted: older history may be incomplete."});
    dialog.UpdateList();
    assert(dialog.m_Items.size() == (telegram ? 3 : 6) && dialog.contains(L"Unavailable due to privacy settings."));
    assert(dialog.contains(L"Read by - unavailable") == telegram);
    protocol->request->onResult({{}, "Receipt request timed out. Retry when connected."});
    dialog.UpdateList();
    assert(dialog.m_Items.size() == (telegram ? 3 : 6) && dialog.contains(L"Request timed out. Retry."));
    if (telegram)
    {
      protocol->request->onResult({{}, "Telegram does not provide per-person delivery receipts. Message is too old for a read timestamp."});
      dialog.UpdateList();
      assert(dialog.contains(L"Read details have expired.") && !dialog.contains(L"Read by - 0"));
      protocol->request->onResult({{}, "Telegram does not provide per-person delivery receipts. Read-by details unavailable for this message (chat type, message age or privacy restrictions)."});
      dialog.UpdateList();
      assert(dialog.contains(L"Unavailable: message age, chat type or privacy limits."));
      protocol->request->onResult({{}, "Telegram reports this message has not been read yet."});
      dialog.UpdateList();
      assert(dialog.contains(L"Read by - 0") && dialog.contains(L"Not read yet."));
    }
    else
    {
      // Actual production receipt-cache wording: missing historical details are
      // unknown, not a positive claim that recipients have not read anything.
      protocol->request->onResult({{}, "WhatsApp: recorded receipts only. Missing entries/times are unknown, not proof of unread or undelivered. Older history may be incomplete."});
      dialog.UpdateList();
      assert(dialog.contains(L"No receipts recorded.") && !dialog.contains(L"Not read yet."));
      protocol->request->onResult({{{"reader", 0, 200}}, "Older unread state is unknown, not proof of undelivered."});
      dialog.UpdateList();
      assert(dialog.contains(L"Read by - 1") && dialog.contains(L"Delivered to - 1") && !dialog.contains(L"Not read yet."));
    }
    dialog.OnBack();
    assert(!dialog.m_Running);
  }
  protocol->request->onResult({{}, "Late response after close"}); // Must not access destroyed UI.
  endwin(); delscreen(screen); fclose(output); fclose(input);
  UiColorConfig::Cleanup();
  AppConfig::Cleanup();
  std::cout << "message info dialog: Telegram read list, WhatsApp read and delivery lists, 100 recipients, filter, resize and close passed\n";
}
