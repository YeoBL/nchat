package main

import (
	"context"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
)

// Called with muteMu held. Replacing a timer also handles remutes and unmuting
// from another device. Expiry rereads the store so an old timer cannot undo a remute.
func (handler *WmEventHandler) publishMuteLocked(client *whatsmeow.Client, jid types.JID, chatID string, until time.Time) {
	if handler.muteClosed || GetClient(handler.connId) != client {
		return
	}
	if handler.muteTimers == nil {
		handler.muteTimers = make(map[string]*time.Timer)
	}
	if timer := handler.muteTimers[chatID]; timer != nil {
		timer.Stop()
		delete(handler.muteTimers, chatID)
	}
	muted := until == store.MutedForever || until.After(time.Now())
	notify := handler.notifyMute
	if notify == nil {
		notify = CWmUpdateMuteNotify
	}
	notify(handler.connId, chatID, BoolToInt(muted))
	if muted && until != store.MutedForever {
		handler.muteTimers[chatID] = time.AfterFunc(time.Until(until), func() {
			handler.refreshChatMute(client, jid)
		})
	}
}

func (handler *WmEventHandler) refreshChatMute(client *whatsmeow.Client, jid types.JID) {
	handler.muteMu.Lock()
	defer handler.muteMu.Unlock()
	if handler.muteClosed || GetClient(handler.connId) != client || client.Store.ChatSettings == nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	settings, err := client.Store.ChatSettings.GetChatSettings(ctx, jid)
	if err == nil && !settings.Found && jid.Server == types.DefaultUserServer && client.Store.LIDs != nil {
		if lid, lookupErr := client.Store.LIDs.GetLIDForPN(ctx, jid); lookupErr == nil && !lid.IsEmpty() {
			settings, err = client.Store.ChatSettings.GetChatSettings(ctx, lid)
		}
	}
	chatID := GetChatId(client, &jid, nil)
	if err != nil || !settings.Found {
		// Retain the known state on a read failure and retry an expiring timer.
		if timer := handler.muteTimers[chatID]; timer != nil {
			timer.Stop()
			handler.muteTimers[chatID] = time.AfterFunc(30*time.Second, func() {
				handler.refreshChatMute(client, jid)
			})
		}
		return
	}
	handler.publishMuteLocked(client, jid, chatID, settings.MutedUntil)
}

func (handler *WmEventHandler) stopMuteTimers() {
	handler.muteMu.Lock()
	defer handler.muteMu.Unlock()
	handler.muteClosed = true
	for _, timer := range handler.muteTimers {
		timer.Stop()
	}
	handler.muteTimers = nil
}

// Cached names include groups. Reconcile them at connect and after full mute
// snapshots, which whatsmeow can apply to its store without individual events.
func (handler *WmEventHandler) refreshKnownChatMutes(client *whatsmeow.Client) {
	mx.Lock()
	ids := make([]string, 0, len(contacts[handler.connId]))
	for id := range contacts[handler.connId] {
		ids = append(ids, id)
	}
	mx.Unlock()
	for _, id := range ids {
		jid, err := types.ParseJID(id)
		if err == nil && (jid.Server == types.GroupServer || jid.Server == types.DefaultUserServer || jid.Server == types.HiddenUserServer) {
			handler.refreshChatMute(client, jid)
		}
	}
}
