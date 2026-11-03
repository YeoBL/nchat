package main

import (
	"path/filepath"
	"testing"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
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
