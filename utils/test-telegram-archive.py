"""Guard the archive routing regression; no profiles, network, or read receipts."""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "lib/tgchat/src/tgchat.cpp").read_text()
position = source.split("[this](td::td_api::updateChatPosition&", 1)[1].split(
    "[this](td::td_api::updateChatNotificationSettings&", 1)[0]
assert "DeleteChatNotify" not in position, "Leaving Main is not proof of deletion"
assert "position->order_ > 0" in position and "UpdateArchivedNotify" in position
new_chat = source.split("[this](td::td_api::updateNewChat&", 1)[1].split(
    "[this](td::td_api::updateChatTitle&", 1)[0]
assert "DeferGetChatDetailsRequest" in new_chat and "SendRequest(details)" in new_chat
fetch = source.split("case GetChatsRequestType:", 1)[1].split("case GetStatusRequestType:", 1)[0]
assert "chatListArchive" in fetch
assert "details->chatIds.assign" in fetch, "Targeted archived chats must bypass Main filtering"
print("PASS: archive routing preserves history; startup and targeted fetch include archived settings")
assert "GetUnreadChatIds" in fetch, "Cached left channels also need membership backfill"
for update in ("updateSupergroup", "updateBasicGroup"):
    handler = source.split("[this](td::td_api::" + update + "&", 1)[1].split("\n  },", 1)[0]
    assert "UpdateMembership" in handler and "DeleteChatNotify" not in handler
print("PASS: live membership updates and cached-unread startup reconciliation are wired without deleting history")
