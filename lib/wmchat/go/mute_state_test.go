package main

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waSyncAction"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
)

type muteSettingsFixture struct {
	store.NoopStore
	mu       sync.Mutex
	settings types.LocalChatSettings
	err      error
}

func (s *muteSettingsFixture) GetChatSettings(context.Context, types.JID) (types.LocalChatSettings, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.settings, s.err
}
func (s *muteSettingsFixture) set(until time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.settings = types.LocalChatSettings{Found: true, MutedUntil: until}
}

func TestWhatsAppMuteDeadlineAndRecovery(t *testing.T) {
	const id = -941
	jid := types.NewJID("155500001-123", types.GroupServer)
	settings := &muteSettingsFixture{}
	self := types.NewJID("155500002", types.DefaultUserServer)
	client := &whatsmeow.Client{Store: &store.Device{ID: &self, ChatSettings: settings}}
	reports := make(chan int, 32)
	handler := &WmEventHandler{connId: id, notifyMute: func(conn int, chat string, muted int) {
		if conn != id || chat != jid.String() {
			t.Errorf("incorrect mute recipient: %d %s", conn, chat)
		}
		reports <- muted
	}}
	mx.Lock()
	clients[id] = client
	contacts[id] = map[string]string{jid.String(): "Synthetic group", "status@broadcast": "Status"}
	mx.Unlock()
	t.Cleanup(func() { handler.stopMuteTimers(); mx.Lock(); delete(clients, id); delete(contacts, id); mx.Unlock() })
	expect := func(want int) {
		t.Helper()
		select {
		case got := <-reports:
			if got != want {
				t.Fatalf("mute=%d, want %d", got, want)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("mute update missing")
		}
	}
	action := func(until time.Time, muted bool) {
		settings.set(until)
		handler.HandleMute(&events.Mute{JID: jid, Action: &waSyncAction.MuteAction{Muted: proto.Bool(muted), MuteEndTimestamp: proto.Int64(until.UnixMilli())}})
	}
	// Real bug: replaying an already-expired muted=true action must restore notifications.
	action(time.Now().Add(-time.Hour), true)
	expect(0)
	settings.set(time.Now().Add(-time.Hour))
	handler.refreshKnownChatMutes(client)
	expect(0) // Existing cache repaired without new history.
	action(store.MutedForever, true)
	expect(1)
	action(time.Time{}, false)
	expect(0)
	// Finite mutes expire while idle, with no incoming event or app restart.
	action(time.Now().Add(50*time.Millisecond), true)
	expect(1)
	expect(0)
	// A remute cancels the old expiry. A full snapshot also restores the current setting.
	action(time.Now().Add(50*time.Millisecond), true)
	expect(1)
	action(store.MutedForever, true)
	expect(1)
	time.Sleep(80 * time.Millisecond)
	select {
	case <-reports:
		t.Fatal("old timer undid remute")
	default:
	}
	settings.set(time.Time{})
	handler.refreshKnownChatMutes(client)
	expect(0)
	// Unknown settings or a failed read cannot invent an unmute.
	settings.mu.Lock()
	settings.settings.Found = false
	settings.mu.Unlock()
	handler.refreshKnownChatMutes(client)
	settings.mu.Lock()
	settings.err = errors.New("synthetic settings failure")
	settings.mu.Unlock()
	handler.refreshKnownChatMutes(client)
	select {
	case <-reports:
		t.Fatal("failed/unknown lookup changed mute state")
	default:
	}
	settings.mu.Lock()
	settings.err = nil
	settings.mu.Unlock()
	action(time.Now().Add(50*time.Millisecond), true)
	expect(1)
	handler.stopMuteTimers()
	time.Sleep(80 * time.Millisecond)
	select {
	case <-reports:
		t.Fatal("cleanup left a live mute timer")
	default:
	}
}
