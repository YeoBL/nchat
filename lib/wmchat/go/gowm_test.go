package main

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
)

func TestFormatPollState(t *testing.T) {
	got := formatPollState(&PollState{Name: "Lunch?", Options: []string{"Today", "Tomorrow"}})
	if want := "[Poll] Lunch?\n• Today\n• Tomorrow"; got != want {
		t.Fatalf("formatPollState() = %q, want %q", got, want)
	}
}

func TestPreferKnownName(t *testing.T) {
	const id = "447700900123@s.whatsapp.net"
	const connID = -1
	AddContactName(connID, id, "Sam Taylor")
	t.Cleanup(func() { delete(contacts, connID) })
	if got := preferKnownName(connID, id, "+447700900123"); got != "Sam Taylor" {
		t.Fatalf("numeric fallback replaced known name: %q", got)
	}
	if got := preferKnownName(connID, id, "New Name"); got != "New Name" {
		t.Fatalf("new contact name was ignored: %q", got)
	}
}

func TestPollVotesAndPersistence(t *testing.T) {
	const connID = -2
	const chatID = "chat@g.us"
	const msgID = "poll-1"
	option := "Tea"
	hash := fmt.Sprintf("%x", whatsmeow.HashPollOptions([]string{option})[0])
	pollStates[connID] = map[string]*PollState{
		pollStateKey(chatID, msgID): {
			Name: "Drink?", Options: []string{option, "Coffee"},
			Votes: map[string][]string{"sam@s.whatsapp.net": {hash}},
		},
	}
	t.Cleanup(func() { delete(pollStates, connID) })
	if got := WmGetPollVotes(connID, chatID, msgID); !strings.Contains(got, `"option":0,"user_id":"sam@s.whatsapp.net"`) {
		t.Fatalf("vote missing: %q", got)
	}
	path := t.TempDir() + "/polls.dat"
	savePollStates(path, pollStates[connID])
	if got := loadPollStates(path)[pollStateKey(chatID, msgID)].Votes["sam@s.whatsapp.net"][0]; got != hash {
		t.Fatalf("restored vote = %q", got)
	}
}

func TestSelectedPollOptions(t *testing.T) {
	state := &PollState{Options: []string{"Tea", "Coffee", "Water"}, MaxChoices: 2, KnownLimit: true}
	options, err := selectedPollOptions(state, "0,2")
	if err != nil || len(options) != 2 || options[0] != "Tea" || options[1] != "Water" {
		t.Fatalf("multi-choice poll selection = %v, %v", options, err)
	}
	if withdrawn, err := selectedPollOptions(state, ""); err != nil || len(withdrawn) != 0 {
		t.Fatalf("empty withdrawal selection rejected: %v, %v", withdrawn, err)
	}
	for _, invalid := range []string{"0,1,2", "0,0", "3", "a"} {
		if _, err := selectedPollOptions(state, invalid); err == nil {
			t.Fatalf("accepted invalid selection %q", invalid)
		}
	}
	state.MaxChoices = 0
	if _, err := selectedPollOptions(state, "0,1,2"); err != nil {
		t.Fatalf("unlimited poll rejected multiple choices: %v", err)
	}
	state.KnownLimit = false
	if _, err := selectedPollOptions(state, "0,1"); err == nil {
		t.Fatal("old cached poll accepted unverified multi-choice vote")
	}
}

func TestPollMetadataBridge(t *testing.T) {
	const connID = -24
	key := pollStateKey("chat", "poll")
	self := types.NewJID("447700900123", types.DefaultUserServer)
	clients[connID] = &whatsmeow.Client{Store: &store.Device{ID: &self}}
	pollStates[connID] = map[string]*PollState{key: {
		Name: "Drink?", Options: []string{"Tea\nwith milk", "Coffee"}, KnownLimit: true, MaxChoices: 2,
		HiddenVoters: true, Votes: map[string][]string{self.String(): {fmt.Sprintf("%x", whatsmeow.HashPollOptions([]string{"Coffee"})[0])}},
	}}
	t.Cleanup(func() { delete(pollStates, connID); delete(clients, connID) })
	var data struct {
		Status   string        `json:"status"`
		Max      int           `json:"max_choices"`
		Options  []string      `json:"options"`
		Votes    []interface{} `json:"votes"`
		Summary  string        `json:"summary"`
		Selected []int         `json:"selected"`
	}
	if err := json.Unmarshal([]byte(WmGetPollVotes(connID, "chat", "poll")), &data); err != nil {
		t.Fatal(err)
	}
	if data.Status != "hidden" || data.Max != 2 || len(data.Options) != 2 ||
		!strings.Contains(data.Options[1], "1 recorded votes") || len(data.Votes) != 0 ||
		!strings.Contains(data.Summary, "1 recorded voters") || len(data.Selected) != 1 || data.Selected[0] != 1 {
		t.Fatalf("incomplete/private poll metadata = %+v", data)
	}
}
