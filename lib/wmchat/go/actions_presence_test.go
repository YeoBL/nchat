package main

import (
	"math"
	"testing"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
)

func TestPresenceSuppliedTimestampsAndAliases(t *testing.T) {
	const connID = -802
	self := types.NewJID("15550001", types.DefaultUserServer)
	peer := types.NewJID("15550002", types.DefaultUserServer)
	client := &whatsmeow.Client{Store: &store.Device{ID: &self, LIDs: &receiptLIDStore{pn: peer}}}
	clients[connID] = client
	t.Cleanup(func() { delete(clients, connID) })
	const supplied = 1793872800
	for _, fixture := range []struct {
		from                 types.JID
		offline              bool
		seen                 time.Time
		wantOnline, wantSeen int
	}{
		{peer, true, time.Unix(supplied, 0), 0, supplied},
		{types.NewJID("90001", types.HiddenUserServer), true, time.Unix(supplied, 0), 0, supplied},
		{peer, false, time.Time{}, 1, 0},
		{peer, false, time.Unix(supplied, 0), 1, 0},
		{peer, true, time.Time{}, 0, 0}, // Hidden/unknown must stay unknown.
		{peer, true, time.Unix(0, 0), 0, 0},
		{peer, true, time.Unix(-1, 0), 0, 0},
		{peer, true, time.Unix(math.MaxInt32+1, 0), 0, 0}, // C int boundary.
	} {
		calls := 0
		handler := &WmEventHandler{connId: connID, notifyStatus: func(id int, user string, online, seen int) {
			calls++
			if id != connID || user != peer.String() || online != fixture.wantOnline || seen != fixture.wantSeen {
				t.Fatalf("presence mapped incorrectly: %d %s %d %d", id, user, online, seen)
			}
		}}
		handler.HandlePresence(&events.Presence{From: fixture.from, Unavailable: fixture.offline, LastSeen: fixture.seen})
		if calls != 1 {
			t.Fatal("presence not forwarded exactly once")
		}
	}
	handler := &WmEventHandler{connId: connID, notifyStatus: func(int, string, int, int) { t.Fatal("non-contact presence forwarded") }}
	handler.HandlePresence(nil)
	for _, from := range []types.JID{types.EmptyJID, types.NewJID("123", types.GroupServer), types.StatusBroadcastJID, types.NewJID("123", types.NewsletterServer)} {
		handler.HandlePresence(&events.Presence{From: from})
	}
}
