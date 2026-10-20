// Run with the same include paths as the nchat target and -Wl,-dead_strip.
#include "../src/uihistoryview.cpp"
#include <cassert>
#include <iostream>

static std::vector<std::string> commands;
bool SysUtil::RunCommand(const std::string& cmd, std::string*)
{
  commands.push_back(cmd);
  return true;
}

int main()
{
  s_PendingThumbnailCommand = "stale image";
  s_ThumbnailDue = std::chrono::steady_clock::now();
  UiHistoryView::SuspendThumbnails();
  assert(s_PendingThumbnailCommand.empty());
  UiHistoryView::FlushThumbnails();
  assert(commands.size() == 1); // Only the clear command, never the stale image.
  UiHistoryView::SuspendThumbnails();
  UiHistoryView::ResumeThumbnails();
  s_PendingThumbnailCommand = "new image";
  UiHistoryView::FlushThumbnails();
  assert(commands.size() == 2); // Nested dialog still suppresses rendering.
  UiHistoryView::ResumeThumbnails();
  s_ThumbnailDue = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  UiHistoryView::FlushThumbnails();
  assert(commands.size() == 2); // Debounce is respected.
  s_ThumbnailDue = std::chrono::steady_clock::now();
  UiHistoryView::FlushThumbnails();
  assert(commands.back() == "new image");
  assert(s_PendingThumbnailCommand.empty());
  UiHistoryView::FlushThumbnails();
  assert(commands.size() == 3); // Render exactly once.
  std::cout << "thumbnail dialog/debounce checks passed\n";
}
