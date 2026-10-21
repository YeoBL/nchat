// Offline checks for the production tools dialog. Only input and service
// responses are substituted; no account, clipboard, helper or device is used.
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
#include "uiclientactiondialog.h"
#include "uicolorconfig.h"
#include "uiconfig.h"
#include "uicontroller.h"
#include "uikeyconfig.h"
#include "uimodel.h"

namespace {
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
{ input.push_back({screen, static_cast<wint_t>(UiKeyConfig::GetKey(name))}); }

void Text(const std::string& text)
{ for (const auto c : text) input.push_back({"", static_cast<wint_t>(c)}); }

struct FakeProtocol : Protocol
{
  std::string GetProfileId() const override { return "Telegram_offline_dialog_ux"; }
  std::string GetProfileDisplayName() const override { return "Offline dialog test"; }
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
    // Model initialization may ask for harmless metadata. Dialog dispatch below
    // handles all synthetic actions; no mutation may escape to this service.
    assert(request->GetMessageType() != SendMessageRequestType);
    assert(request->GetMessageType() != MarkMessageReadRequestType);
    assert(request->GetMessageType() != DeleteMessageRequestType);
    assert(request->GetMessageType() != ClientActionRequestType);
  }
};

ClientActionItem Form(const std::string& title, bool quick, bool confirm)
{
  ClientActionItem item;
  item.title = title;
  item.operation = "synthetic.action";
  item.fields = {{"query", "Query text", ""}};
  if (quick) item.params["_quickForm"] = "1";
  item.confirm = confirm;
  return item;
}

void CheckFocusAndExcerpts(UiModel& model, const std::shared_ptr<FakeProtocol>& protocol)
{
  UiClientActionDialog::Page page;
  page.result.title = "A menu heading";
  page.result.text = "This heading is information, not an action.";
  auto item = Form("First useful action", false, false);
  item.fields.clear();
  for (int i = 0; i < 200; ++i) item.detail += "Long synthetic detail line " + std::to_string(i) + "\n";
  page.result.items.push_back(item);
  {
    UiDialogParams params(&model, "Tools", 0.8, 0.7);
    UiClientActionDialog dialog(params, page, true);
    assert(dialog.m_Index >= 0 && dialog.m_Index < static_cast<int>(dialog.m_Visible.size()));
    assert(dialog.m_Visible[dialog.m_Index] == 0 && "New menu must focus an action, not its heading or Back");
    assert(dialog.m_Items.size() < 12 && "List details must be bounded excerpts");
    assert(dialog.CurrentPage().result.items.front().detail == item.detail && "Excerpt must not destroy full content");
  }
  {
    auto request = std::make_shared<ClientActionRequest>();
    request->operation = "synthetic.load";
    UiDialogParams params(&model, "Tools", 0.8, 0.7);
    UiClientActionDialog dialog(params, {}, true, protocol, request,
      [page](const std::string&, const std::map<std::string, std::string>&,
             std::function<void(ClientActionResult)> done)
      { done(page.result); return true; });
    assert(dialog.OnTimer());
    assert(dialog.m_Visible[dialog.m_Index] == 0 && "Fetched menu must focus first action, not Back");
  }
  {
    UiClientActionDialog::Page preview;
    preview.result.text = item.detail;
    preview.focusText = true;
    preview.result.items.push_back(item);
    UiDialogParams params(&model, "Full preview", 0.8, 0.7);
    UiClientActionDialog dialog(params, preview, false);
    assert(dialog.m_Items.size() >= 200 && "Full-message preview must remain complete and scrollable");
    assert(dialog.m_Index == 0 && "Full previews must focus the text beginning, not the actions below it");
    Key("Long synthetic detail line 0", "cancel");
    dialog.Run();
    assert(input.empty() && "The initial preview viewport must display the first text line");
  }
  {
    auto request = std::make_shared<ClientActionRequest>();
    request->operation = "local.message";
    UiDialogParams params(&model, "Fetched preview", 0.8, 0.7);
    auto preview = page.result;
    preview.text = "FIRST PREVIEW LINE\n" + item.detail;
    UiClientActionDialog dialog(params, {}, true, protocol, request,
      [preview](const std::string&, const std::map<std::string, std::string>&,
        std::function<void(ClientActionResult)> done) { done(preview); return true; });
    assert(dialog.OnTimer() && dialog.m_Index == 0);
    Key("FIRST PREVIEW LINE", "cancel");
    dialog.Run();
    assert(input.empty());
  }
}

void CheckReadOnlyAndMutationForms(UiModel& model)
{
  auto search = Form("Read-only query", true, false);
  Text("needle");
  Key("Query text", "ok");
  assert(UiClientActionDialog::PrepareAction(&model, search));
  assert(input.empty() && "Read-only query must not request optional fields or confirmation");
  assert(search.params.at("query") == "needle" && !search.params.count("_quickForm"));

  // Explicit mutation confirmation wins even if the quick-form marker leaks in.
  auto mutation = Form("Confirmed mutation", true, true);
  Key("Review: Confirmed mutation", "ok"); // Cancel, never Confirm, is the default.
  assert(!UiClientActionDialog::PrepareAction(&model, mutation));
  assert(input.empty() && mutation.fields.front().value.empty());

  // Older mutation forms may rely on the implicit review, not confirm=true.
  auto legacy = Form("Legacy mutation", false, false);
  legacy.fields.push_back({"optional", "Optional value", "sensible default"});
  input.push_back({"Review: Legacy mutation", 0});
  Text("Query text");
  Key("", "end");
  Key("", "ok");
  Text("draft");
  Key("Query text", "ok");
  Key("Review: Legacy mutation", "cancel");
  assert(!UiClientActionDialog::PrepareAction(&model, legacy));
  assert(input.empty() && legacy.fields.front().value == "draft");
  assert(legacy.params.at("optional") == "sensible default" &&
    "Optional/default fields must not demand editing before an action can be reviewed");
}

void CheckRetryPreservesFields(UiModel& model, const std::shared_ptr<FakeProtocol>& protocol)
{
  int attempts = 0;
  const auto dispatch = [&attempts](const std::string& operation,
    const std::map<std::string, std::string>& params, std::function<void(ClientActionResult)> done)
  {
    ClientActionResult result;
    if (operation == "synthetic.menu")
    {
      result.title = "Retry test menu";
      result.items.push_back(Form("Try a read-only query", true, false));
    }
    else
    {
      assert(operation == "synthetic.action");
      assert(params.at("query") == "retained words");
      assert(!params.count("_quickForm"));
      ++attempts;
      result.success = false;
      result.title = "Synthetic validation error";
      result.text = "Nothing was sent. Go Back and correct the query.";
    }
    done(std::move(result));
    return true;
  };
  Key("Retry test menu", "ok");
  Text("retained words");
  Key("Query text", "ok");
  Key("Synthetic validation error", "left");
  Key("Retry test menu", "ok");
  Key("retained words", "cancel");
  Key("Retry test menu", "cancel");
  UiClientActionDialog::RunActions(&model, protocol, "Retry test", "synthetic.menu", {}, {}, dispatch);
  assert(attempts == 1 && input.empty());
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
  char temp[] = "/tmp/nchat-tools-dialog-ux.XXXXXX";
  const auto root = mkdtemp(temp);
  assert(root);
  FileUtil::SetApplicationDir(root);
  FileUtil::InitTempDir();
  {
    std::ofstream config(std::string(root) + "/ui.conf");
    config << "thumbnail_command=/usr/bin/true\n";
  }
  AppConfig::Init();
  UiConfig::Init();
  UiConfig::SetBool("desktop_notify_enabled", false);
  UiConfig::SetBool("typing_status_share", false);
  FILE* output = tmpfile();
  FILE* source = tmpfile();
  assert(output && source);
  SCREEN* terminal = newterm("xterm-256color", output, source);
  assert(terminal);
  set_term(terminal);
  assert(resizeterm(45, 140) != ERR);
  UiColorConfig::Init();
  UiKeyConfig::Init(false);
  auto protocol = std::make_shared<FakeProtocol>();
  {
    UiModel model;
    model.AddProtocol(protocol);
    model.Init();
    CheckFocusAndExcerpts(model, protocol);
    CheckReadOnlyAndMutationForms(model);
    CheckRetryPreservesFields(model, protocol);
    model.Cleanup();
  }
  assert(input.empty());
  UiKeyConfig::Cleanup();
  UiColorConfig::Cleanup();
  endwin();
  delscreen(terminal);
  fclose(source);
  fclose(output);
  UiConfig::Cleanup();
  AppConfig::Cleanup();
  FileUtil::RmDir(root);
  std::cout << "PASS: tools action focus, bounded excerpts/full preview, quick read-only query, mutation review, retry field preservation; no service mutations\n";
}
