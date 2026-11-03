package main

import (
	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/types"
)

// A previous inventory may contain group/channel names absent from the local
// address book. Restore these even when WhatsApp cannot be reached at startup.
func publishCachedContactNames(connID int, client *whatsmeow.Client,
	notify func(int, string, string, string, int, int, int)) {
	mx.Lock()
	names := make(map[string]string, len(contacts[connID]))
	for id, name := range contacts[connID] {
		if id != "" && name != "" {
			names[id] = name
		}
	}
	mx.Unlock()
	remaining := len(names)
	for id, name := range names {
		remaining--
		mode := NotifyCache
		if remaining == 0 {
			mode = NotifySendCached
		}
		jid, _ := types.ParseJID(id)
		notify(connID, id, name, PhoneFromUserId(id), BoolToInt(IsSelfUser(client, id)),
			BoolToInt(jid.Server == types.HiddenUserServer), mode)
	}
}
