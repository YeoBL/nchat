package main

import (
	"context"
	"slices"
	"strings"
	"testing"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waCommon"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/proto/waWeb"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"google.golang.org/protobuf/proto"
)

func TestHistoricalPollRecovery(t *testing.T) {
	chat, _ := types.ParseJID("123@g.us")
	self, _ := types.ParseJID("447700900100@s.whatsapp.net")
	voter, _ := types.ParseJID("447700900123@s.whatsapp.net")
	update := &waWeb.PollUpdate{
		PollUpdateMessageKey: &waCommon.MessageKey{Participant: proto.String(voter.String())},
		Vote:                 &waE2E.PollVoteMessage{SelectedOptions: [][]byte{{1, 2}}},
		SenderTimestampMS:    proto.Int64(1000),
	}
	if got := historicalPollVoter(chat, self, update); got != voter {
		t.Fatalf("historical voter = %s, want %s", got, voter)
	}
	state := &PollState{}
	if !setRecordedPollVote(state, voter.String(), update.GetVote().GetSelectedOptions(), 1000) ||
		state.Votes[voter.String()][0] != "0102" {
		t.Fatal("historical vote was not recovered")
	}
	if !setRecordedPollVote(state, voter.String(), nil, 2000) {
		t.Fatal("vote withdrawal was not recorded")
	}
	if setRecordedPollVote(state, voter.String(), update.GetVote().GetSelectedOptions(), 1000) ||
		len(state.Votes[voter.String()]) != 0 {
		t.Fatal("old history resurrected a withdrawn vote")
	}
	path := t.TempDir() + "/polls.dat"
	savePollStates(path, map[string]*PollState{"poll": state})
	if got := loadPollStates(path)["poll"].VoteTimes[voter.String()]; got != 2000 {
		t.Fatalf("vote recency was not persisted: %d", got)
	}
	update.PollUpdateMessageKey = &waCommon.MessageKey{FromMe: proto.Bool(true)}
	if got := historicalPollVoter(chat, self, update); got != self {
		t.Fatalf("own historical voter = %s, want %s", got, self)
	}
	update.PollUpdateMessageKey = &waCommon.MessageKey{}
	if got := historicalPollVoter(chat, self, update); !got.IsEmpty() {
		t.Fatalf("anonymous group vote assigned to %s", got)
	}
	if got := historicalPollVoter(voter, self, update); got != voter {
		t.Fatalf("direct-chat historical voter = %s, want %s", got, voter)
	}
}

func TestHiddenPollVoters(t *testing.T) {
	const connID = -96
	pollStates[connID] = map[string]*PollState{
		pollStateKey("chat", "poll"): {
			HiddenVoters: true,
			Options:      []string{"Tea"},
			Votes:        map[string][]string{"private@s.whatsapp.net": {"0102"}},
		},
	}
	t.Cleanup(func() { delete(pollStates, connID) })
	if got := WmGetPollVotes(connID, "chat", "poll"); !strings.Contains(got, `"status":"hidden"`) || strings.Contains(got, "voter@") {
		t.Fatalf("hidden poll disclosed voter data: %q", got)
	}
}

func TestPollVoterAliases(t *testing.T) {
	state := &PollState{Votes: map[string][]string{"person@lid": {"old"}, "447700900123@s.whatsapp.net": {}}, VoteTimes: map[string]int64{"person@lid": 1000, "447700900123@s.whatsapp.net": 2000}}
	normalizeRecordedPollVoters(state, func(id string) string {
		if id == "person@lid" {
			return "447700900123@s.whatsapp.net"
		}
		return id
	})
	if len(state.Votes) != 1 || len(state.Votes["447700900123@s.whatsapp.net"]) != 0 || state.VoteTimes["447700900123@s.whatsapp.net"] != 2000 {
		t.Fatal("linked identity duplicated or resurrected a withdrawn vote")
	}
}

func TestPollMergeSameTimeAndUnknownTime(t *testing.T) {
	for _, choices := range [][][][]byte{
		{{{2}, {1}, {1}}, {{2}, {3}}},
		{{{2}, {3}}, {{2}, {1}, {1}}},
		{{{1}}, nil},
		{nil, {{1}}},
	} {
		state := &PollState{}
		for _, selected := range choices {
			setRecordedPollVote(state, "voter", selected, 1000)
		}
		want := []string{}
		if len(choices[0]) > 1 {
			want = []string{"02"}
		}
		if !slices.Equal(state.Votes["voter"], want) {
			t.Fatalf("equal-time choices %x merged to %v, want %v", choices, state.Votes["voter"], want)
		}
		if setRecordedPollVote(state, "voter", [][]byte{{4}}, 0) {
			t.Fatal("timestamp-free history overwrote a known-time result")
		}
	}
	state := &PollState{Votes: map[string][]string{"voter": {"01", "01"}}}
	setRecordedPollVote(state, "voter", [][]byte{{2}}, 0)
	if len(state.Votes["voter"]) != 0 || state.VoteTimes["voter"] != 0 {
		t.Fatal("conflicting unknown-time history invented an ordering")
	}
	setRecordedPollVote(state, "voter", [][]byte{{2}, {2}, {1}}, 1000)
	if !slices.Equal(state.Votes["voter"], []string{"01", "02"}) {
		t.Fatal("newer result was not sorted and deduplicated")
	}
}

type historyLIDStore struct {
	store.LIDStore
	lid, phone types.JID
}

func (s *historyLIDStore) GetPNForLID(_ context.Context, lid types.JID) (types.JID, error) {
	if lid == s.lid {
		return s.phone, nil
	}
	return types.EmptyJID, nil
}

func TestPollCanonicalAliasesAndNormalizationOnlyChanges(t *testing.T) {
	self := types.NewJID("447700900100", types.DefaultUserServer)
	selfLID := types.NewJID("100", types.HiddenUserServer)
	lid := types.NewJID("200", types.HiddenUserServer)
	phone := types.NewJID("447700900200", types.DefaultUserServer)
	client := &whatsmeow.Client{Store: &store.Device{ID: &self, LID: selfLID,
		LIDs: &historyLIDStore{lid: lid, phone: phone}}}
	for raw, want := range map[string]string{
		"100:7@lid": self.String(), "200:3@lid": phone.String(),
		"447700900200:8@s.whatsapp.net": phone.String(),
	} {
		if got := canonicalPollVoterID(client, raw); got != want {
			t.Fatalf("canonical %q = %q, want %q", raw, got, want)
		}
	}
	for _, reverse := range []bool{false, true} {
		first, second := []string{"01"}, []string{}
		if reverse {
			first, second = second, first
		}
		state := &PollState{Votes: map[string][]string{lid.String(): first, phone.String(): second},
			VoteTimes: map[string]int64{lid.String(): 1000, phone.String(): 1000}}
		if !normalizeRecordedPollVoters(state, func(raw string) string { return canonicalPollVoterID(client, raw) }) ||
			len(state.Votes) != 1 || len(state.Votes[phone.String()]) != 0 {
			t.Fatal("equal-time alias normalization revived a withdrawal or failed to report its change")
		}
		if normalizeRecordedPollVoters(state, func(raw string) string { return canonicalPollVoterID(client, raw) }) {
			t.Fatal("normalization was not idempotent")
		}
		path := t.TempDir() + "/polls.dat"
		savePollStates(path, map[string]*PollState{"poll": state})
		if loaded := loadPollStates(path)["poll"]; len(loaded.Votes) != 1 || len(loaded.Votes[phone.String()]) != 0 {
			t.Fatal("normalization-only alias migration did not survive persistence")
		}
	}
}

func TestHistoricalPollSuppliedUpdatesAndPartialMerge(t *testing.T) {
	chat := types.NewJID("group", types.GroupServer)
	self := types.NewJID("self", types.DefaultUserServer)
	voter := types.NewJID("voter", types.DefaultUserServer)
	hash := whatsmeow.HashPollOptions([]string{"Tea"})[0]
	update := func(participant string, choices [][]byte, timestamp int64) *waWeb.PollUpdate {
		return &waWeb.PollUpdate{PollUpdateMessageKey: &waCommon.MessageKey{Participant: proto.String(participant)},
			Vote: &waE2E.PollVoteMessage{SelectedOptions: choices}, SenderTimestampMS: proto.Int64(timestamp)}
	}
	inputs := []*waWeb.PollUpdate{
		nil, {}, update("voter@g.us", [][]byte{hash}, 1000), update("garbage", [][]byte{hash}, 1000),
		update("voter@s.whatsapp.net@extra", [][]byte{hash}, 1000),
		update(voter.String(), [][]byte{{1}}, 1000),
		update(voter.String(), [][]byte{hash, hash}, 1000),
		update(voter.String(), nil, 2000), // A later withdrawal must survive old snapshots.
		update(voter.String(), [][]byte{hash}, 1500),
	}
	inputs = append(inputs, update("late@s.whatsapp.net", [][]byte{hash}, 0))
	inputs[len(inputs)-1].ServerTimestampMS = proto.Int64(3000)
	recovered := historicalPollUpdates(chat, self, "poll", inputs, func(id string) string { return id })
	if len(recovered) != 4 || recovered[0].event.Info.ID == "" || recovered[0].event.Info.ID !=
		historicalPollUpdates(chat, self, "poll", inputs, func(id string) string { return id })[0].event.Info.ID {
		t.Fatalf("invalid updates accepted or missing stable IDs: %+v", recovered)
	}
	state := &PollState{Votes: map[string][]string{"omitted@s.whatsapp.net": {"existing"}}}
	for _, vote := range recovered {
		recordPollUpdate(state, nil, vote)
	}
	if len(state.Votes[voter.String()]) != 0 || state.VoteTimes[voter.String()] != 2000 ||
		len(state.Votes["omitted@s.whatsapp.net"]) != 1 || state.VoteTimes["late@s.whatsapp.net"] != 3000 {
		t.Fatalf("partial history erased voters or resurrected withdrawals: %+v", state)
	}
}

func TestHistoricalPollBeforeCreationIsQueuedDurably(t *testing.T) {
	const connID = -7299
	chat := types.NewJID("group", types.GroupServer)
	self := types.NewJID("self", types.DefaultUserServer)
	clients[connID] = &whatsmeow.Client{Store: &store.Device{ID: &self}}
	paths[connID] = t.TempDir()
	t.Cleanup(func() {
		delete(clients, connID)
		delete(paths, connID)
		delete(pollStates, connID)
		delete(pendingPollVotes, connID)
	})
	message := &waWeb.WebMessageInfo{PollUpdates: []*waWeb.PollUpdate{{
		PollUpdateMessageKey: &waCommon.MessageKey{Participant: proto.String("voter@s.whatsapp.net")},
		Vote:                 &waE2E.PollVoteMessage{SelectedOptions: whatsmeow.HashPollOptions([]string{"Tea"})},
		SenderTimestampMS:    proto.Int64(1000),
	}}}
	handler := &WmEventHandler{connId: connID}
	info := types.MessageInfo{ID: "poll", MessageSource: types.MessageSource{Chat: chat}}
	handler.RecoverHistoricalPollVotes(info, message)
	// Repeated snapshots with a device alias or duplicate choices are still one
	// logical update, not extra entries consuming the bounded durable queue.
	message.PollUpdates[0].PollUpdateMessageKey.Participant = proto.String("voter:3@s.whatsapp.net")
	message.PollUpdates[0].Vote.SelectedOptions = append(message.PollUpdates[0].Vote.SelectedOptions,
		message.PollUpdates[0].Vote.SelectedOptions[0])
	handler.RecoverHistoricalPollVotes(info, message)
	_, restored, err := loadPollCache(pollStorePath(paths[connID]))
	if err != nil || len(restored) != 1 || restored[0].vote == nil || len(restored[0].vote.GetSelectedOptions()) != 1 {
		t.Fatalf("supplied history before creation was lost or duplicated: %+v, %v", restored, err)
	}
}
