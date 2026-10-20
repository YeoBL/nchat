package main

import (
	"bytes"
	"context"
	"encoding/gob"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"google.golang.org/protobuf/proto"
)

func TestPollCacheRestartReplayAndWithdrawal(t *testing.T) {
	const connID = -7351
	root := t.TempDir()
	creator := types.NewJID("creator", types.DefaultUserServer)
	voter := types.NewJID("voter", types.DefaultUserServer)
	chat := types.NewJID("chat", types.GroupServer)
	secrets := &pendingPollSecretStore{secret: make([]byte, 32), sender: creator}
	client := &whatsmeow.Client{Store: &store.Device{ID: &voter, MsgSecrets: secrets}}
	clients[connID], paths[connID] = client, root
	pollStates[connID] = map[string]*PollState{}
	t.Cleanup(func() {
		delete(clients, connID)
		delete(paths, connID)
		delete(pollStates, connID)
		delete(pendingPollVotes, connID)
		delete(pollStoreBlocked, connID)
		delete(pollStoreWriteErrors, connID)
	})
	pollInfo := types.MessageInfo{ID: "poll", MessageSource: types.MessageSource{Chat: chat, Sender: creator, IsGroup: true}}
	message, err := client.BuildPollVote(context.Background(), &pollInfo, []string{"Tea"})
	if err != nil {
		t.Fatal(err)
	}
	message.PollUpdateMessage.SenderTimestampMS = proto.Int64(1000)
	event := pendingTestEvent("encrypted", 1000)
	event.Info.Chat, event.Message = chat, message
	secrets.secret = nil
	notified := 0
	handler := &WmEventHandler{connId: connID, notifyMessage: func(gotConn int, gotChat, messageID, senderID, text string,
		fromMe int, quotedID, fileID, filePath string, fileStatus, sent, read, edited int) {
		notified++
		if gotConn != connID || gotChat != chat.String() || messageID != "poll" || read != -1 || edited != 1 ||
			!strings.Contains(text, "1 recorded vote") {
			t.Fatalf("restored poll changed read state or target: %d %s %s %d %d %q", gotConn, gotChat, messageID, read, edited, text)
		}
	}}
	handler.HandlePollUpdate(event.Info, event.Message)
	path := pollStorePath(root)
	data, err := os.ReadFile(path)
	if err != nil || !bytes.HasPrefix(data, []byte(pollStoreMagic)) {
		t.Fatalf("early vote not persisted: %v", err)
	}
	info, err := os.Stat(path)
	if err != nil || info.Mode().Perm() != 0600 {
		t.Fatalf("private cache permissions: %v, %v", info, err)
	}
	states, pending, err := loadPollCache(path)
	if err != nil || len(pending) != 1 {
		t.Fatalf("restart pending load: %v, %d", err, len(pending))
	}
	pollStates[connID], pendingPollVotes[connID] = states, pending // Simulate process restart.
	key := pollStateKey(chat.String(), "poll")
	pollStates[connID][key] = &PollState{Name: "Drink?", Options: []string{"Tea", "Coffee"}}
	savePollCache(connID)
	if replayPendingPollVotes(connID, client, key) {
		t.Fatal("undecryptable vote was applied")
	}
	_, stillPending, err := loadPollCache(path)
	if err != nil || len(stillPending) != 1 {
		t.Fatal("missing secret discarded a durable early vote")
	}
	secrets.secret = make([]byte, 32)
	if handler.ReplayCachedPolls() != 1 {
		t.Fatal("persisted encrypted vote was not recovered")
	}
	if notified != 1 {
		t.Fatal("restored poll did not refresh the terminal through the historical notifier")
	}
	states, pending, err = loadPollCache(path)
	if err != nil || len(pending) != 0 || len(states[key].Votes[voter.String()]) != 1 {
		t.Fatalf("replayed state not atomically persisted: %v, %d, %+v", err, len(pending), states[key])
	}
	withdrawal := pendingTestEvent("withdrawal", 2000)
	withdrawal.Info.Chat = chat
	mx.Lock()
	queuePendingPollVote(connID, key, withdrawal, &waE2E.PollVoteMessage{})
	savePollCache(connID)
	mx.Unlock()
	states, pending, err = loadPollCache(path)
	if err != nil || len(pending) != 1 || pending[0].vote == nil || len(pending[0].vote.GetSelectedOptions()) != 0 {
		t.Fatal("empty decoded withdrawal did not survive restart")
	}
	pollStates[connID], pendingPollVotes[connID] = states, pending
	// Opening an existing cached poll also replays restored updates without needing
	// to receive the original creation a second time or sending anything.
	if output := WmGetPollVotes(connID, chat.String(), "poll"); !strings.Contains(output, "0 recorded voters") {
		t.Fatalf("read-only poll opening did not apply withdrawal: %s", output)
	}
	states, pending, err = loadPollCache(path)
	if err != nil || len(pending) != 0 || len(states[key].Votes[voter.String()]) != 0 || states[key].VoteTimes[voter.String()] != 2000 {
		t.Fatal("withdrawal or queue removal was not durable")
	}
	// A phone may replay the old encrypted vote after this restart.
	mx.Lock()
	queuePendingPollVote(connID, key, event, nil)
	savePollCache(connID)
	mx.Unlock()
	replayPendingPollVotes(connID, client, key)
	if len(pollStates[connID][key].Votes[voter.String()]) != 0 {
		t.Fatal("old replay resurrected withdrawn vote")
	}
}

func TestPollCacheLegacyMigrationAndUnsafeFiles(t *testing.T) {
	root := t.TempDir()
	path := pollStorePath(root)
	legacy := map[string]*PollState{"poll": {Name: "Legacy", Votes: map[string][]string{"voter": {}}, VoteTimes: map[string]int64{"voter": 20}}}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		t.Fatal(err)
	}
	if err := gob.NewEncoder(f).Encode(legacy); err != nil {
		t.Fatal(err)
	}
	f.Close()
	states, pending, err := loadPollCache(path)
	if err != nil || len(pending) != 0 || states["poll"].VoteTimes["voter"] != 20 {
		t.Fatal("legacy cache was not migrated")
	}
	if err := writePollCache(path, states, pending); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(path)
	info, _ := os.Stat(path)
	if !bytes.HasPrefix(data, []byte(pollStoreMagic)) || info.Mode().Perm() != 0600 {
		t.Fatal("migration was not versioned/private")
	}
	outside := filepath.Join(root, "outside")
	if err := os.WriteFile(outside, []byte("untouched"), 0600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(root, "linked-polls")
	if err := os.Symlink(outside, link); err != nil {
		t.Fatal(err)
	}
	if _, _, err := loadPollCache(link); err == nil {
		t.Fatal("followed poll cache symlink")
	}
	if err := writePollCache(link, states, nil); err == nil {
		t.Fatal("replaced unsafe poll cache target")
	}
	if data, _ := os.ReadFile(outside); string(data) != "untouched" {
		t.Fatal("unsafe cache changed another file")
	}
}

func TestPollCacheCorruptionAndFutureVersionArePreserved(t *testing.T) {
	for _, future := range []bool{false, true} {
		root := t.TempDir()
		path := pollStorePath(root)
		var original bytes.Buffer
		original.WriteString(pollStoreMagic)
		if future {
			if err := gob.NewEncoder(&original).Encode(storedPollCache{Version: 999, States: map[string]*PollState{}}); err != nil {
				t.Fatal(err)
			}
		} else {
			original.WriteString("truncated")
		}
		if err := os.WriteFile(path, original.Bytes(), 0600); err != nil {
			t.Fatal(err)
		}
		states, pending, err := loadPollCache(path)
		if err == nil {
			t.Fatal("unsupported cache accepted")
		}
		const connID = -7352
		paths[connID], pollStates[connID], pendingPollVotes[connID], pollStoreBlocked[connID] = root, states, pending, true
		savePollCache(connID)
		data, _ := os.ReadFile(path)
		if !bytes.Equal(data, original.Bytes()) {
			t.Fatal("unreadable historical cache was overwritten")
		}
		if !strings.Contains(WmGetPollVotes(connID, "chat", "poll"), "preserved") {
			t.Fatal("unreadable cache not explained")
		}
		delete(paths, connID)
		delete(pollStates, connID)
		delete(pendingPollVotes, connID)
		delete(pollStoreBlocked, connID)
	}
}

func TestPollCachePendingBoundsAndMetadata(t *testing.T) {
	path := pollStorePath(t.TempDir())
	event := pendingTestEvent("vote", 1000)
	event.Info.Chat = types.NewJID("chat", types.GroupServer)
	key := pollStateKey(event.Info.Chat.String(), "poll")
	update := pendingPollVote{key, event, &waE2E.PollVoteMessage{}}
	if err := writePollCache(path, map[string]*PollState{}, make([]pendingPollVote, maxPendingPollVotes+1)); err == nil {
		t.Fatal("persisted unbounded pending queue")
	}
	stored, err := storedPendingVote(update)
	if err != nil {
		t.Fatal(err)
	}
	stored.Vote = make([]byte, maxPendingPollVoteSize+1)
	if _, err := restoredPendingVote(stored); err == nil {
		t.Fatal("loaded oversized decoded vote")
	}
	stored, _ = storedPendingVote(update)
	stored.Key = pollStateKey("chat@g.us", "wrong-poll")
	if _, err := restoredPendingVote(stored); err == nil {
		t.Fatal("loaded mismatched poll identity")
	}
	stored, _ = storedPendingVote(update)
	stored.HasVote = false
	stored.Message = []byte{255}
	if _, err := restoredPendingVote(stored); err == nil {
		t.Fatal("loaded corrupt encrypted update")
	}
	// Private cache restores millisecond ordering precisely, not truncated seconds.
	stored, _ = storedPendingVote(update)
	stored.Timestamp = time.UnixMilli(1234).UnixMilli()
	got, err := restoredPendingVote(stored)
	if err != nil || got.event.Info.Timestamp.UnixMilli() != 1234 {
		t.Fatal("lost update timestamp precision")
	}
}

func TestPollCacheHistoryUpgradesEncryptedResend(t *testing.T) {
	const connID = -7353
	root := t.TempDir()
	voter := types.NewJID("voter", types.DefaultUserServer)
	client := &whatsmeow.Client{Store: &store.Device{ID: &voter}}
	clients[connID], paths[connID], pollStates[connID] = client, root, map[string]*PollState{}
	t.Cleanup(func() {
		delete(clients, connID)
		delete(paths, connID)
		delete(pollStates, connID)
		delete(pendingPollVotes, connID)
		delete(pollStoreWriteErrors, connID)
	})
	event := pendingTestEvent("same-stanza", 1234)
	event.Info.Chat = types.NewJID("chat", types.GroupServer)
	key := pollStateKey(event.Info.Chat.String(), "poll")
	mx.Lock()
	queuePendingPollVote(connID, key, event, nil)
	queuePendingPollVote(connID, key, event, &waE2E.PollVoteMessage{}) // Decoded withdrawal from phone history.
	queuePendingPollVote(connID, key, event, nil)                      // An encrypted resend must not undo the upgrade.
	savePollCache(connID)
	mx.Unlock()
	states, pending, err := loadPollCache(pollStorePath(root))
	if err != nil || len(pending) != 1 || pending[0].vote == nil {
		t.Fatalf("decoded history was dropped by queue deduplication: %v, %+v", err, pending)
	}
	pollStates[connID], pendingPollVotes[connID] = states, pending
	pollStates[connID][key] = &PollState{Options: []string{"Tea"}}
	if !replayPendingPollVotes(connID, client, key) || pollStates[connID][key].VoteTimes[voter.String()] != 1234 {
		t.Fatal("decoded history could not replay without any encryption secret")
	}
}
