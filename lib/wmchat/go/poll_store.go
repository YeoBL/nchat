package main

import (
	"bytes"
	"encoding/gob"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
)

const pollStoreMagic = "NCHAT-POLLS\n"
const pollStoreVersion = 1
const maxPollStoreBytes = 128 * 1024 * 1024

// Only the metadata required by WhatsMeow's poll decryption is persisted. No
// original-message secret is copied into this existing private poll cache.
type storedPendingPollVote struct {
	Key, Chat, Sender, ID string
	FromMe, Group         bool
	Timestamp             int64
	Message, Vote         []byte
	HasVote               bool
}

type storedPollCache struct {
	Version int
	States  map[string]*PollState
	Pending []storedPendingPollVote
}

var pollStoreBlocked = map[int]bool{}
var pollStoreWriteErrors = map[int]bool{}

func storedPendingVote(update pendingPollVote) (storedPendingPollVote, error) {
	var stored storedPendingPollVote
	if update.event == nil || update.event.Message == nil || update.key == "" {
		return stored, errors.New("invalid pending poll update")
	}
	message, err := proto.Marshal(update.event.Message)
	if err != nil || len(message) > maxPendingPollVoteSize {
		return stored, errors.New("invalid pending poll payload")
	}
	stored = storedPendingPollVote{Key: update.key, Chat: update.event.Info.Chat.String(),
		Sender: update.event.Info.Sender.String(), ID: update.event.Info.ID,
		FromMe: update.event.Info.IsFromMe, Group: update.event.Info.IsGroup,
		Timestamp: update.event.Info.Timestamp.UnixMilli(), Message: message, HasVote: update.vote != nil}
	if update.vote != nil {
		stored.Vote, err = proto.Marshal(update.vote)
		if err != nil || len(stored.Vote) > maxPendingPollVoteSize {
			return storedPendingPollVote{}, errors.New("invalid decoded poll payload")
		}
	}
	return stored, nil
}

func restoredPendingVote(stored storedPendingPollVote) (pendingPollVote, error) {
	var update pendingPollVote
	if len(stored.Message) > maxPendingPollVoteSize || len(stored.Vote) > maxPendingPollVoteSize ||
		len(stored.Key) > 1024 || len(stored.ID) > 256 || len(stored.Sender) > 256 || len(stored.Chat) > 256 {
		return update, errors.New("oversized pending poll update")
	}
	chat, chatErr := types.ParseJID(stored.Chat)
	sender, senderErr := types.ParseJID(stored.Sender)
	message := &waE2E.Message{}
	if chatErr != nil || senderErr != nil || chat.IsEmpty() || sender.IsEmpty() ||
		proto.Unmarshal(stored.Message, message) != nil || message.GetPollUpdateMessage() == nil {
		return update, errors.New("invalid pending poll message")
	}
	pollID := message.GetPollUpdateMessage().GetPollCreationMessageKey().GetID()
	if pollID == "" || !strings.HasSuffix(stored.Key, "\x00"+pollID) {
		return update, errors.New("pending poll key mismatch")
	}
	update = pendingPollVote{key: stored.Key, event: &events.Message{Info: types.MessageInfo{
		ID: stored.ID, Timestamp: time.UnixMilli(stored.Timestamp), MessageSource: types.MessageSource{
			Chat: chat, Sender: sender, IsFromMe: stored.FromMe, IsGroup: stored.Group}}, Message: message}}
	if stored.HasVote {
		update.vote = &waE2E.PollVoteMessage{}
		if proto.Unmarshal(stored.Vote, update.vote) != nil {
			return pendingPollVote{}, errors.New("invalid decoded poll update")
		}
	}
	return update, nil
}

func loadPollCache(path string) (map[string]*PollState, []pendingPollVote, error) {
	empty := make(map[string]*PollState)
	before, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return empty, nil, nil
	}
	if err != nil || !before.Mode().IsRegular() || before.Size() > maxPollStoreBytes {
		return empty, nil, errors.New("poll cache is unreadable, unsafe or too large")
	}
	f, err := os.Open(path)
	if err != nil {
		return empty, nil, err
	}
	defer f.Close()
	after, err := f.Stat()
	if err != nil || !after.Mode().IsRegular() || !os.SameFile(before, after) {
		return empty, nil, errors.New("poll cache changed while opening")
	}
	data, err := io.ReadAll(io.LimitReader(f, maxPollStoreBytes+1))
	if err != nil || len(data) > maxPollStoreBytes {
		return empty, nil, errors.New("poll cache read failed")
	}
	if !bytes.HasPrefix(data, []byte(pollStoreMagic)) {
		var legacy map[string]*PollState
		if err := gob.NewDecoder(bytes.NewReader(data)).Decode(&legacy); err != nil || legacy == nil {
			return empty, nil, errors.New("poll cache is corrupt; retained without overwriting")
		}
		return legacy, nil, nil
	}
	var stored storedPollCache
	if err := gob.NewDecoder(bytes.NewReader(data[len(pollStoreMagic):])).Decode(&stored); err != nil ||
		stored.Version != pollStoreVersion || stored.States == nil || len(stored.Pending) > maxPendingPollVotes {
		return empty, nil, errors.New("poll cache version/data unsupported; retained without overwriting")
	}
	var pending []pendingPollVote
	counts := map[string]int{}
	for _, encoded := range stored.Pending {
		update, err := restoredPendingVote(encoded)
		if err != nil || counts[update.key] >= maxPendingPollVotesPerPoll {
			return empty, nil, errors.New("poll cache pending update invalid; retained without overwriting")
		}
		counts[update.key]++
		pending = append(pending, update)
	}
	return stored.States, pending, nil
}

func writePollCache(path string, states map[string]*PollState, pending []pendingPollVote) error {
	if len(pending) > maxPendingPollVotes {
		return errors.New("pending poll cache exceeds bound")
	}
	if info, err := os.Lstat(path); err == nil && !info.Mode().IsRegular() {
		return errors.New("refusing unsafe poll cache target")
	} else if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	stored := storedPollCache{Version: pollStoreVersion, States: states}
	if stored.States == nil {
		stored.States = map[string]*PollState{}
	}
	for _, update := range pending {
		encoded, err := storedPendingVote(update)
		if err != nil {
			return err
		}
		stored.Pending = append(stored.Pending, encoded)
	}
	var data bytes.Buffer
	data.WriteString(pollStoreMagic)
	if err := gob.NewEncoder(&data).Encode(stored); err != nil {
		return err
	}
	if data.Len() > maxPollStoreBytes {
		return errors.New("poll cache exceeds supported size; previous file preserved")
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".polls-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(data.Bytes()); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	if err = os.Rename(f.Name(), path); err != nil {
		return err
	}
	dir, err := os.Open(filepath.Dir(path))
	if err != nil {
		return err
	}
	defer dir.Close()
	return dir.Sync()
}

// Called with mx held: states and early encrypted/decrypted updates are one
// atomic snapshot, so a crash cannot commit one while losing the other.
func savePollCache(connID int) {
	if paths[connID] == "" || pollStoreBlocked[connID] {
		return
	}
	if err := writePollCache(pollStorePath(paths[connID]), pollStates[connID], pendingPollVotes[connID]); err != nil {
		pollStoreWriteErrors[connID] = true
		LOG_WARNING(fmt.Sprintf("save poll cache failed: %v", err))
	} else {
		delete(pollStoreWriteErrors, connID)
	}
}

// Decryption can wait on a secret arriving later. Keep the queued originals in
// the persisted snapshot until successfully applied, including during replay.
func replayPendingPollVotes(connID int, client *whatsmeow.Client, key string) bool {
	if client == nil || client.Store == nil {
		return false
	}
	mx.Lock()
	if pollStates[connID][key] == nil {
		mx.Unlock()
		return false
	}
	var pending []pendingPollVote
	for _, update := range pendingPollVotes[connID] {
		if update.key == key {
			pending = append(pending, update)
		}
	}
	mx.Unlock()
	ready := decryptPendingPollVotes(client, pending)
	if len(ready) == 0 {
		return false
	}
	mx.Lock()
	defer mx.Unlock()
	state := pollStates[connID][key]
	if state == nil {
		return false
	}
	changed := normalizeRecordedPollVoters(state, func(raw string) string { return canonicalPollVoterID(client, raw) })
	processed := map[*events.Message]bool{}
	for _, update := range ready {
		if recordPollUpdate(state, client, update) {
			changed = true
		}
		processed[update.event] = true
	}
	var retained []pendingPollVote
	for _, update := range pendingPollVotes[connID] {
		if !processed[update.event] {
			retained = append(retained, update)
		}
	}
	pendingPollVotes[connID] = retained
	savePollCache(connID)
	return changed
}

// Reconnection is also a local recovery opportunity: cached polls must not wait
// for the user to open the voter dialog before restored votes are applied.
func (handler *WmEventHandler) ReplayCachedPolls() int {
	client := GetClient(handler.connId)
	if client == nil {
		return 0
	}
	mx.Lock()
	keys := map[string]bool{}
	for _, update := range pendingPollVotes[handler.connId] {
		if pollStates[handler.connId][update.key] != nil {
			keys[update.key] = true
		}
	}
	mx.Unlock()
	changed := 0
	for key := range keys {
		if !replayPendingPollVotes(handler.connId, client, key) {
			continue
		}
		separator := strings.IndexByte(key, 0)
		if separator < 0 {
			continue
		}
		mx.Lock()
		state := pollStates[handler.connId][key]
		if state == nil {
			mx.Unlock()
			continue
		}
		text, sender := formatPollState(state), state.SenderID
		fromMe, sent, read := state.FromMe, state.TimeSent, state.IsRead
		mx.Unlock()
		importer := *handler
		importer.importedHistory = true
		importer.NotifyNewMessage(handler.connId, key[:separator], key[separator+1:], sender, text,
			BoolToInt(fromMe), "", "", "", FileStatusNone, sent, BoolToInt(read), BoolToInt(true))
		changed++
	}
	return changed
}

// Retained for small state-only migration/persistence fixtures.
func loadPollStates(path string) map[string]*PollState {
	states, _, _ := loadPollCache(path)
	return states
}

func savePollStates(path string, states map[string]*PollState) {
	if err := writePollCache(path, states, nil); err != nil {
		LOG_WARNING(fmt.Sprintf("save polls failed: %v", err))
	}
}
