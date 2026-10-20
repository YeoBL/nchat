#pragma once
#include <td/telegram/td_api.h>

inline bool TelegramIsMember(const td::td_api::ChatMemberStatus& status)
{
  switch (status.get_id())
  {
    case td::td_api::chatMemberStatusCreator::ID:
      return static_cast<const td::td_api::chatMemberStatusCreator&>(status).is_member_;
    case td::td_api::chatMemberStatusAdministrator::ID:
    case td::td_api::chatMemberStatusMember::ID:
      return true;
    case td::td_api::chatMemberStatusRestricted::ID:
      return static_cast<const td::td_api::chatMemberStatusRestricted&>(status).is_member_;
    case td::td_api::chatMemberStatusLeft::ID:
    case td::td_api::chatMemberStatusBanned::ID:
      return false;
    default:
      return true; // An unknown future status is not proof of leaving.
  }
}
