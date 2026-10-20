package main

import (
	"strings"
	"testing"

	"go.mau.fi/whatsmeow/proto/waCommon"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/proto/waWeb"
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
