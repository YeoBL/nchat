package main

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
)

func TestCachedNamesPublishedBeforeNetwork(t *testing.T) {
	const connID = -801
	self := types.NewJID("15550001", types.DefaultUserServer)
	client := &whatsmeow.Client{Store: &store.Device{ID: &self}}
	want := map[string]string{
		self.String(): "Me", "12030001@g.us": "Study Group",
		"15550002@s.whatsapp.net": "Sam Taylor", "90001@lid": "Sam Alias",
		"12030002@newsletter": "Updates",
	}
	path := filepath.Join(t.TempDir(), "contacts.dat")
	if err := SaveMap(path, want); err != nil {
		t.Fatal(err)
	}
	var err error
	contacts[connID], err = LoadMap(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { delete(contacts, connID) })
	seen, flushed := map[string]bool{}, 0
	publishCachedContactNames(connID, client, func(id int, jid, name, phone string, isSelf, alias, mode int) {
		if id != connID || want[jid] != name || seen[jid] {
			t.Fatalf("incorrect restored contact %q", jid)
		}
		seen[jid] = true
		if (isSelf == 1) != (jid == self.String()) || (alias == 1) != (jid == "90001@lid") {
			t.Fatalf("incorrect identity flags for %q", jid)
		}
		if phone != PhoneFromUserId(jid) {
			t.Fatalf("incorrect phone fallback for %q", jid)
		}
		if mode == NotifySendCached {
			flushed++
			if len(seen) != len(want) {
				t.Fatal("flushed before all saved names were restored")
			}
		} else if mode != NotifyCache {
			t.Fatal("unexpected notification mode")
		}
		// The callback may update contacts: publishing must not hold mx or iterate
		// the live map. Neither update should join this startup snapshot.
		AddContactName(connID, "later@g.us", "Later Group")
	})
	if len(seen) != len(want) || flushed != 1 {
		t.Fatal("incomplete cached-name batch")
	}
	delete(contacts, connID)
	publishCachedContactNames(connID, client, func(int, string, string, string, int, int, int) {
		t.Fatal("empty cache emitted a replacement")
	})
}

func TestLiveGroupNamesOnJoinAndRename(t *testing.T) {
	if os.Getenv("NCHAT_TEST_NATIVE_LOGS") != "1" {
		t.Skip("run utils/test-whatsapp-group-names.sh for the native event-routing fixture")
	}
	const connID = -804
	group := types.NewJID("12030004", types.GroupServer)
	self := types.NewJID("15550001", types.DefaultUserServer)
	clients[connID] = &whatsmeow.Client{Store: &store.Device{ID: &self}}
	t.Cleanup(func() {
		mx.Lock()
		delete(clients, connID)
		delete(contacts, connID)
		mx.Unlock()
	})

	var published []string
	messages := 0
	handler := &WmEventHandler{
		connId: connID,
		notifyContact: func(id int, jid, name, phone string, isSelf, alias, mode int) {
			if id != connID || jid != group.String() || phone != "" || isSelf != 0 || alias != 0 || mode != NotifyDirect {
				t.Fatal("group update was not an immediate partial contact notification")
			}
			if GetContactName(connID, jid) != name {
				t.Fatal("notification preceded the in-memory name update")
			}
			published = append(published, name)
		},
		notifyMessage: func(id int, chat, messageID, sender, text string, fromMe int,
			quotedID, fileID, filePath string, fileStatus, sent, read, edited int) {
			messages++
			if chat != group.String() || text != "[Changed group name to Renamed Group]" {
				t.Fatal("rename lost its existing system message")
			}
		},
	}
	// A message may have inserted the numeric chat ID before join metadata arrives.
	AddContactName(connID, group.String(), group.String())
	handler.HandleEvent(&events.JoinedGroup{GroupInfo: types.GroupInfo{
		JID: group, GroupName: types.GroupName{Name: "Study Group 🇸🇬\n2026"},
	}})
	if len(published) != 1 || published[0] != "Study Group 🇸🇬 2026" || messages != 0 {
		t.Fatal("joining an active session did not immediately replace the numeric name")
	}
	handler.HandleEvent(&events.GroupInfo{JID: group, Sender: &self,
		Timestamp: time.Unix(100, 0), Name: &types.GroupName{Name: "Renamed Group"}})
	if len(published) != 2 || published[1] != "Renamed Group" || messages != 1 {
		t.Fatal("live rename did not refresh both contact metadata and its system message")
	}
	// A duplicate/partial join cannot erase the title already supplied by WhatsApp.
	handler.HandleEvent(&events.JoinedGroup{GroupInfo: types.GroupInfo{JID: group}})
	if len(published) != 3 || published[2] != "Renamed Group" {
		t.Fatal("partial group metadata overwrote the known title")
	}
	AddContactName(connID, group.String(), group.String())
	peer := types.NewJID("15550002", types.DefaultUserServer)
	AddContactName(connID, peer.String(), "Sam")
	handler.HandleEvent(&events.JoinedGroup{GroupInfo: types.GroupInfo{JID: group,
		Participants: []types.GroupParticipant{{JID: self}, {JID: peer}}}})
	if len(published) != 4 || published[3] != "Sam" {
		t.Fatal("participant fallback did not replace the numeric placeholder")
	}
	handler.HandleEvent((*events.JoinedGroup)(nil))
	handler.HandleEvent((*events.GroupInfo)(nil))
	handler.HandleEvent(&events.JoinedGroup{})
	handler.HandleEvent(&events.JoinedGroup{GroupInfo: types.GroupInfo{JID: self}})
	if len(published) != 4 || messages != 1 {
		t.Fatal("invalid metadata published a group title")
	}
}
