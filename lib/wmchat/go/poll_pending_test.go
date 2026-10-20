package main

import (
	"context"
	"fmt"
	"testing"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waCommon"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
)

type pendingPollSecretStore struct {
	secret []byte
	sender types.JID
}

func (s *pendingPollSecretStore) GetMessageSecret(context.Context, types.JID, types.JID, types.MessageID) ([]byte, types.JID, error) {
	return s.secret, s.sender, nil
}
func (s *pendingPollSecretStore) PutMessageSecret(context.Context, types.JID, types.JID, types.MessageID, []byte) error {
	return nil
}
func (s *pendingPollSecretStore) PutMessageSecrets(context.Context, []store.MessageSecretInsert) error {
	return nil
}

func pendingTestEvent(id string, timestamp int64) *events.Message {
	return &events.Message{
		Info: types.MessageInfo{ID: id, MessageSource: types.MessageSource{Sender: types.NewJID("voter", types.DefaultUserServer)}, Timestamp: time.UnixMilli(timestamp)},
		Message: &waE2E.Message{PollUpdateMessage: &waE2E.PollUpdateMessage{
			PollCreationMessageKey: &waCommon.MessageKey{ID: proto.String("poll")}, SenderTimestampMS: proto.Int64(timestamp),
		}},
	}
}

func TestPendingPollVotesReplayAndWithdrawal(t *testing.T) {
	const connID = -7201
	t.Cleanup(func() { delete(pendingPollVotes, connID); delete(clients, connID) })
	creator := types.NewJID("creator", types.DefaultUserServer)
	voter := types.NewJID("voter", types.DefaultUserServer)
	chat := types.NewJID("chat", types.GroupServer)
	secrets := &pendingPollSecretStore{secret: make([]byte, 32), sender: creator}
	client := &whatsmeow.Client{Store: &store.Device{ID: &voter, MsgSecrets: secrets}}
	clients[connID] = client
	pollInfo := types.MessageInfo{ID: "poll", MessageSource: types.MessageSource{Chat: chat, Sender: creator, IsGroup: true}}
	message, err := client.BuildPollVote(context.Background(), &pollInfo, []string{"Tea"})
	if err != nil {
		t.Fatal(err)
	}
	message.PollUpdateMessage.SenderTimestampMS = proto.Int64(1000)
	event := pendingTestEvent("vote", 1000)
	event.Info.Chat = chat
	event.Message = message
	key := pollStateKey(chat.String(), "poll")
	secrets.secret = nil // The encrypted update precedes the original poll/secret.
	if _, err := client.DecryptPollVote(context.Background(), event); err == nil {
		t.Fatal("early vote unexpectedly decrypted without original poll secret")
	}
	handler := &WmEventHandler{connID}
	handler.HandlePollUpdate(event.Info, event.Message)
	// A decrypted withdrawal can also precede the UI's poll state.
	queuePendingPollVote(connID, key, pendingTestEvent("withdraw", 2000), &waE2E.PollVoteMessage{})
	// A stale replay must not resurrect a withdrawn vote, regardless of arrival order.
	queuePendingPollVote(connID, key, pendingTestEvent("old", 1500), &waE2E.PollVoteMessage{SelectedOptions: [][]byte{{1}}})
	secrets.secret = make([]byte, 32)
	withdrawal, err := client.BuildPollVote(context.Background(), &pollInfo, []string{})
	if err != nil {
		t.Fatalf("native withdrawal could not be built: %v", err)
	}
	withdrawEvent := *event
	withdrawEvent.Message = withdrawal
	decodedWithdrawal, err := client.DecryptPollVote(context.Background(), &withdrawEvent)
	if err != nil || len(decodedWithdrawal.GetSelectedOptions()) != 0 {
		t.Fatalf("native withdrawal payload is not empty: %v, %v", decodedWithdrawal, err)
	}
	ready := decryptPendingPollVotes(client, takePendingPollVotes(connID, key))
	if len(ready) != 3 || len(ready[0].vote.GetSelectedOptions()) != 1 {
		t.Fatalf("early encrypted vote was not recovered: %+v", ready)
	}
	state := &PollState{}
	for _, update := range ready {
		recordPollUpdate(state, client, update)
	}
	if len(state.Votes[voter.String()]) != 0 || state.VoteTimes[voter.String()] != 2000 {
		t.Fatalf("stale pending vote resurrected withdrawal: %+v", state)
	}
	if len(pendingPollVotes[connID]) != 0 || len(takePendingPollVotes(connID, key)) != 0 {
		t.Fatal("replayed votes remained pending")
	}
	// A newer live vote recorded during replay wins over all old queued updates.
	setRecordedPollVote(state, voter.String(), [][]byte{{2}}, 3000)
	for _, update := range ready {
		recordPollUpdate(state, client, update)
	}
	if state.Votes[voter.String()][0] != "02" || state.VoteTimes[voter.String()] != 3000 {
		t.Fatal("queued replay overwrote a newer live vote")
	}
}

func TestPendingPollVotesBoundsAndIsolation(t *testing.T) {
	const connID, otherConn = -7202, -7203
	t.Cleanup(func() { delete(pendingPollVotes, connID); delete(pendingPollVotes, otherConn) })
	queuePendingPollVote(otherConn, "other", pendingTestEvent("other", 1), nil)
	for i := 0; i < maxPendingPollVotesPerPoll+10; i++ {
		queuePendingPollVote(connID, "one", pendingTestEvent(fmt.Sprint(i), int64(i)), nil)
	}
	if got := len(pendingPollVotes[connID]); got != maxPendingPollVotesPerPoll {
		t.Fatalf("per-poll queue grew to %d", got)
	}
	for i := 0; i < maxPendingPollVotes+10; i++ {
		queuePendingPollVote(connID, fmt.Sprint(i), pendingTestEvent(fmt.Sprint(i), int64(i)), nil)
	}
	if got := len(pendingPollVotes[connID]); got != maxPendingPollVotes {
		t.Fatalf("per-connection queue grew to %d", got)
	}
	if got := len(takePendingPollVotes(otherConn, "other")); got != 1 {
		t.Fatalf("another account's vote was lost: %d", got)
	}
	lastKey := fmt.Sprint(maxPendingPollVotes + 9)
	last := pendingTestEvent(lastKey, 9999)
	queuePendingPollVote(connID, lastKey, last, nil)
	if got := len(pendingPollVotes[connID]); got != maxPendingPollVotes {
		t.Fatal("resent update duplicated in queue")
	}
	ready := takePendingPollVotes(connID, lastKey)
	if len(ready) != 1 || len(pendingPollVotes[connID]) != maxPendingPollVotes-1 {
		t.Fatal("draining one poll lost another poll's pending update")
	}
	oversize := pendingTestEvent("large", 1)
	oversize.Message.PollUpdateMessage.Vote = &waE2E.PollEncValue{EncPayload: make([]byte, maxPendingPollVoteSize+1)}
	queuePendingPollVote(otherConn, "large", oversize, nil)
	if len(pendingPollVotes[otherConn]) != 0 {
		t.Fatal("unbounded encrypted payload was retained")
	}
	clone := pendingTestEvent("clone", 1)
	queuePendingPollVote(otherConn, "clone", clone, &waE2E.PollVoteMessage{SelectedOptions: [][]byte{{1}}})
	clone.Message.PollUpdateMessage.SenderTimestampMS = proto.Int64(999)
	if got := takePendingPollVotes(otherConn, "clone")[0].event.Message.GetPollUpdateMessage().GetSenderTimestampMS(); got != 1 {
		t.Fatalf("queued message was mutated by its caller: %d", got)
	}
}
