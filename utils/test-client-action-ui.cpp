// c++ -std=c++14 -D_XOPEN_SOURCE_EXTENDED=1 -Isrc -Ilib/common/src -Ilib/ncutil/src -Ibuild/generated \
//   -I/opt/homebrew/opt/ncurses/include utils/test-client-action-ui.cpp \
//   -Lbuild/lib -lncutil -Wl,-rpath,$PWD/build/lib -o /tmp/test-client-action-ui
#include "../src/uiclientactiondialog.h"

#include <cassert>
#include <clocale>
#include <iostream>

int main()
{
  std::setlocale(LC_ALL, "");
  using namespace ClientActionUi;
  const std::wstring wrapped = L"abcdefghij";
  assert(MoveFieldLine(wrapped, 5, 2, true) == 7);
  assert(MoveFieldLine(wrapped, 5, 7, false) == 2);
  assert(LayoutField(L"ab\n", 5, 3).row == 1);
  assert(MoveFieldLine(L"abc\nxy", 5, 2, true) == 6);
  assert(LayoutField(L"中x", 5, 1).col == 2);
  assert(MoveFieldLine(L"中x\nab", 5, 1, true) == 5);
  assert(LayoutField(L"", 1, 0).lines.size() == 1);

  ClientActionItem item;
  item.title = "Gallery";
  item.detail = "Files and voice notes";
  item.operation = "private.internal.operation";
  assert(Matches(item, "gallery"));
  assert(Matches(item, "voice"));
  assert(!Matches(item, "internal"));
  const auto params = MergeParams({{"chatId", "original"}, {"msgId", "message"}, {"threadId", "topic"}},
    {{"chatId", "selected"}});
  assert(params.at("chatId") == "selected" && params.at("msgId") == "message" && params.at("threadId") == "topic");
  assert(DisplayText("one\n\ttwo\x1b", true) == "one\n two ");
  assert(DisplayText("one\ntwo") == "one two");
  const auto compact = WrapText("one\ntwo\nthree\nfour", 10, 3);
  assert(compact.size() == 3 && compact.back().back() == L'…');
  assert(WrapText("one\ntwo\nthree\nfour", 10).size() == 4);
  assert(WrapText("abcdef", 1, 1) == std::vector<std::wstring>{L"…"});
  std::cout << "account-tools search, field layout and terminal-text checks passed\n";
}
