#include "../lib/tgchat/src/tgmembership.h"
#include <cassert>
#include <iostream>

int main()
{
  using namespace td::td_api;
  assert(!TelegramIsMember(chatMemberStatusLeft()));
  assert(!TelegramIsMember(chatMemberStatusBanned()));
  assert(TelegramIsMember(chatMemberStatusMember()));
  assert(TelegramIsMember(chatMemberStatusAdministrator()));
  chatMemberStatusCreator owner;
  chatMemberStatusRestricted restricted;
  for (bool member : {false, true})
  {
    owner.is_member_ = member;
    restricted.is_member_ = member;
    assert(TelegramIsMember(owner) == member);
    assert(TelegramIsMember(restricted) == member);
  }
  std::cout << "PASS: actual Telegram left/banned/member/admin/restricted/creator status handling\n";
}
