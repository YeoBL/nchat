#include "../lib/tgchat/src/forumreadstate.h"
#include <cassert>
#include <iostream>

int main()
{
  ForumReadState state;
  assert(!state.Track({1, 10}, 100));
  assert(!state.Track({1, 10}, 110));
  assert(!state.Track({1, 20}, 90));
  assert(!state.Track({2, 10}, 95));
  assert(!state.Track({1, 10}, 100)); // Duplicate message, only one notification.
  assert(state.Read({1, 10}, 100) == std::vector<int64_t>{100});
  assert(state.Read({1, 10}, 99).empty()); // Delayed cursor cannot regress.
  assert(state.Track({1, 10}, 100)); // History replay stays read.
  assert(!state.Track({1, 10}, 110));
  assert(state.Read({1, 10}, 200) == std::vector<int64_t>{110});
  assert(state.Read({1, 20}, 90) == std::vector<int64_t>{90});
  assert(state.Read({2, 10}, 95) == std::vector<int64_t>{95});
  assert(state.Read({3, 1}, 500).empty()); // Update before cache recovery.
  assert(state.Track({3, 1}, 499));
  assert(!state.Track({3, 1}, 501));
  assert(state.Read({3, 1}, 501) == std::vector<int64_t>{501});
  std::cout << "PASS: forum partial reads, topic/chat isolation, duplicate and delayed updates, startup replay\n";
}
