#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include <td/telegram/td_api.h>

// TDLib reports chat overrides and scope defaults separately. Unknown defaults
// remain muted until fetched, rather than briefly exposing suppressed alerts.
class TelegramMuteState
{
public:
  enum Scope { Unknown, Private, Group, Channel };
  using Changes = std::vector<std::pair<int64_t, bool>>;

  static Scope GetScope(const td::td_api::ChatType* type)
  {
    if (!type) return Unknown;
    using namespace td::td_api;
    if (type->get_id() == chatTypePrivate::ID || type->get_id() == chatTypeSecret::ID) return Private;
    if (type->get_id() == chatTypeBasicGroup::ID) return Group;
    if (type->get_id() == chatTypeSupergroup::ID)
      return static_cast<const chatTypeSupergroup&>(*type).is_channel_ ? Channel : Group;
    return Unknown;
  }

  static Scope GetScope(const td::td_api::NotificationSettingsScope* scope)
  {
    if (!scope) return Unknown;
    using namespace td::td_api;
    if (scope->get_id() == notificationSettingsScopePrivateChats::ID) return Private;
    if (scope->get_id() == notificationSettingsScopeGroupChats::ID) return Group;
    if (scope->get_id() == notificationSettingsScopeChannelChats::ID) return Channel;
    return Unknown;
  }

  bool SetChat(const td::td_api::chat& chat)
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto& state = m_Chats[chat.id_];
    state.scope = GetScope(chat.type_.get());
    SetSettings(state, chat.notification_settings_.get());
    return Muted(state);
  }

  bool SetChatSettings(int64_t chatId, const td::td_api::chatNotificationSettings* settings)
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto& state = m_Chats[chatId];
    SetSettings(state, settings);
    return Muted(state);
  }

  bool IsMuted(int64_t chatId) const
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto found = m_Chats.find(chatId);
    return found == m_Chats.end() || Muted(found->second);
  }

  bool HasChatScope(int64_t chatId) const
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto found = m_Chats.find(chatId);
    return found != m_Chats.end() && found->second.scope != Unknown;
  }

  bool IsResolved(int64_t chatId) const
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto chat = m_Chats.find(chatId);
    if (chat == m_Chats.end() || !chat->second.known) return false;
    if (!chat->second.useDefault) return true;
    const auto scope = m_Scopes.find(chat->second.scope);
    return scope != m_Scopes.end() && scope->second.known;
  }

  uint64_t BeginScopeRequest(Scope scope)
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto& state = m_Scopes[scope];
    if (scope == Unknown || state.known || state.pending) return 0;
    state.pending = true;
    return ++state.generation;
  }

  Changes SetScope(Scope scope, const td::td_api::scopeNotificationSettings* settings, uint64_t request = 0)
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (scope == Unknown) return {};
    auto& state = m_Scopes[scope];
    if (request && request != state.generation) return {}; // A newer pushed update wins.
    state.pending = false;
    if (!settings) return {}; // Failed initial lookup: stay conservative and permit retry.
    Changes previous;
    for (const auto& chat : m_Chats)
      if (chat.second.scope == scope && chat.second.useDefault) previous.emplace_back(chat.first, Muted(chat.second));
    state.known = true;
    state.muted = settings->mute_for_ > 0;
    ++state.generation;
    Changes changes;
    for (const auto& chat : previous)
    {
      const bool muted = Muted(m_Chats.at(chat.first));
      if (muted != chat.second) changes.emplace_back(chat.first, muted);
    }
    return changes;
  }

private:
  struct Chat { Scope scope = Unknown; bool known = false, useDefault = true, muted = true; };
  struct ScopeSettings { bool known = false, muted = true, pending = false; uint64_t generation = 0; };

  static void SetSettings(Chat& chat, const td::td_api::chatNotificationSettings* settings)
  {
    chat.known = settings != nullptr;
    chat.useDefault = !settings || settings->use_default_mute_for_;
    chat.muted = !settings || settings->mute_for_ > 0;
  }

  bool Muted(const Chat& chat) const
  {
    if (!chat.known) return true;
    if (!chat.useDefault) return chat.muted;
    auto found = m_Scopes.find(chat.scope);
    return found == m_Scopes.end() || !found->second.known || found->second.muted;
  }

  mutable std::mutex m_Mutex;
  std::map<int64_t, Chat> m_Chats;
  std::map<Scope, ScopeSettings> m_Scopes;
};
