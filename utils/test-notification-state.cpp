// clang++ -std=c++14 utils/test-notification-state.cpp -o /tmp/nchat-notification-test && /tmp/nchat-notification-test
#include "../src/notificationstate.h"
#include <cassert>
#include <iostream>

int main()
{
  {
    NotificationState topics;
    const NotificationState::Chat chat{"Telegram_fixture", "forum"};
    topics.Add(chat, "other-topic", 100);
    topics.Add(chat, "read-topic", 200);
    assert(topics.TakeBell());
    topics.Read(chat, "read-topic");
    assert(!topics.TakeClear());
    topics.Read(chat, "other-topic");
    assert(topics.TakeClear());
  }
  const NotificationState::Chat telegram{"tg", "chat"}, whatsapp{"wa", "chat"};
  NotificationState state;
  assert(state.Add(telegram, "3939")); // 99
  state.Read(telegram, "", "3939");
  assert(!state.TakeBell()); // Read before UI tick must cancel queued bell.
  assert(!state.TakeClear());
  assert(!state.Add(telegram, "3939")); // Stale history cannot replay it.

  assert(state.Add(telegram, "313030")); // 100: length-aware numeric order
  assert(state.TakeBell());
  state.Read(telegram, "", "3939");
  assert(!state.TakeClear()); // Partial inbox marker leaves newer alert.
  state.Read(telegram, "outgoing-message");
  assert(!state.TakeClear()); // Someone reading our message is irrelevant.
  assert(state.Add(whatsapp, "incoming"));
  assert(state.TakeBell());
  state.Read(telegram, "", "313030");
  assert(!state.TakeClear()); // Another chat is still unread.
  state.Read(whatsapp, "incoming");
  assert(state.TakeClear());
  assert(!state.TakeClear());
  assert(!state.TakeBell());

  assert(state.Add(whatsapp, "first"));
  assert(state.Add(whatsapp, "second"));
  assert(state.TakeBell());
  state.Read(whatsapp, "first");
  assert(!state.TakeClear());
  state.Read(whatsapp, ""); // Chat read on another device, no UI history needed.
  assert(state.TakeClear());

  assert(state.Add(telegram, "313031"));
  assert(state.TakeBell());
  state.Read(telegram, "313031");
  assert(state.Add(telegram, "313032")); // New message supersedes queued clear.
  assert(!state.TakeClear());
  assert(state.TakeBell());
  std::cout << "notification ordering, partial reads, duplicates, and multiple chats: passed\n";

  NotificationState burst;
  for (int i = 0; i < 10000; ++i)
  {
    const std::string id = std::to_string(i);
    if (i % 2) burst.Read(whatsapp, id); // Receipt won the delivery race.
    assert(burst.Add(whatsapp, id) == (i % 2 == 0));
    assert(!burst.Add(whatsapp, id));
  }
  assert(burst.TakeBell());
  for (int i = 0; i < 9998; i += 2) burst.Read(whatsapp, std::to_string(i));
  assert(!burst.TakeClear());
  burst.Read(whatsapp, "9998");
  assert(burst.TakeClear());
  assert(burst.PendingChats() == 0);
  std::cout << "10,000-message burst with early receipts and duplicate deliveries: passed\n";
  NotificationState progress;
  assert(progress.Add(whatsapp, "orphan", 100));
  assert(progress.Add(whatsapp, "later", 200));
  assert(progress.Add(whatsapp, "newest", 300));
  assert(progress.TakeBell());
  progress.Read(whatsapp, "later");
  assert(!progress.TakeClear()); // Newer unread must survive read progress.
  progress.Read(whatsapp, "outgoing");
  assert(!progress.TakeClear());
  progress.Read(whatsapp, "newest");
  assert(progress.TakeClear());
  assert(!progress.Add(whatsapp, "late-cache-record", 150));
  assert(progress.Add(whatsapp, "future", 400));
  assert(progress.TakeBell());
  progress.Read(whatsapp, "old-read", "", 200);
  assert(!progress.TakeClear());
  assert(progress.Add(whatsapp, "deleted", 500));
  progress.Remove(whatsapp, "deleted");
  assert(!progress.TakeClear()); // Deletion is not read progress.
  progress.Read(whatsapp, "future");
  assert(progress.TakeClear());
  std::cout << "chat read progress: orphan retirement, newer unread retention, stale cache and deletion passed\n";
}
