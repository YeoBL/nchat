package main

import (
	"context"
	"fmt"
	"sort"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waWeb"
	"go.mau.fi/whatsmeow/types"
)

func canonicalPollVoterID(client *whatsmeow.Client, raw string) string {
	if client == nil || client.Store == nil {
		return raw
	}
	if IsSelfUser(client, raw) && client.Store.ID != nil {
		return StrFromJid(*client.Store.ID)
	}
	jid, err := types.ParseJID(raw)
	if err == nil && jid.Server == types.HiddenUserServer && client.Store.LIDs != nil {
		if phone, err := client.Store.LIDs.GetPNForLID(context.Background(), jid); err == nil && !phone.IsEmpty() {
			return phone.ToNonAD().String()
		}
	}
	return raw
}

// A phone number and its linked-device identity represent the same voter.
func normalizeRecordedPollVoters(state *PollState, canonical func(string) string) {
	if state == nil || len(state.Votes) == 0 {
		return
	}
	ids := make([]string, 0, len(state.Votes))
	for id := range state.Votes {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		target := canonical(id)
		if target == "" || target == id {
			continue
		}
		if _, exists := state.Votes[target]; !exists || state.VoteTimes[id] > state.VoteTimes[target] {
			state.Votes[target] = state.Votes[id]
			if state.VoteTimes == nil {
				state.VoteTimes = map[string]int64{}
			}
			state.VoteTimes[target] = state.VoteTimes[id]
		}
		delete(state.Votes, id)
		delete(state.VoteTimes, id)
	}
}

// The phone may resend old snapshots after newer live votes, including withdrawals.
func setRecordedPollVote(state *PollState, voter string, selected [][]byte, timestamp int64) bool {
	if state == nil || voter == "" || timestamp < 0 {
		return false
	}
	if previous, ok := state.VoteTimes[voter]; ok && timestamp <= previous {
		return false
	}
	if state.Votes == nil {
		state.Votes = make(map[string][]string)
	}
	if state.VoteTimes == nil {
		state.VoteTimes = make(map[string]int64)
	}
	hashes := make([]string, 0, len(selected))
	for _, option := range selected {
		hashes = append(hashes, fmt.Sprintf("%x", option))
	}
	state.Votes[voter] = hashes
	state.VoteTimes[voter] = timestamp
	return true
}

func historicalPollVoter(chat, self types.JID, update *waWeb.PollUpdate) types.JID {
	key := update.GetPollUpdateMessageKey()
	if key == nil {
		return types.EmptyJID
	}
	if key.GetFromMe() {
		return self.ToNonAD()
	}
	if key.GetParticipant() != "" {
		voter, err := types.ParseJID(key.GetParticipant())
		if err == nil {
			return voter.ToNonAD()
		}
		return types.EmptyJID
	}
	if chat.Server == types.DefaultUserServer || chat.Server == types.HiddenUserServer {
		return chat.ToNonAD()
	}
	return types.EmptyJID
}

// History sync carries already-decrypted poll votes on the original poll message.
// It is supplementary history from the phone, not a complete server voter list.
func (handler *WmEventHandler) RecoverHistoricalPollVotes(info types.MessageInfo, message *waWeb.WebMessageInfo) {
	if len(message.GetPollUpdates()) == 0 {
		return
	}
	client := GetClient(handler.connId)
	if client == nil || client.Store.ID == nil {
		return
	}
	chatID := GetChatId(client, &info.Chat, &info.Sender)
	mx.Lock()
	state := pollStates[handler.connId][pollStateKey(chatID, info.ID)]
	if state == nil {
		mx.Unlock()
		return
	}
	normalizeRecordedPollVoters(state, func(raw string) string { return canonicalPollVoterID(client, raw) })
	changed := false
	for _, update := range message.GetPollUpdates() {
		if update.GetVote() == nil {
			continue
		}
		voter := historicalPollVoter(info.Chat, *client.Store.ID, update)
		if voter.IsEmpty() {
			continue
		}
		voterID := canonicalPollVoterID(client, voter.String())
		timestamp := update.GetSenderTimestampMS()
		if timestamp <= 0 {
			timestamp = update.GetServerTimestampMS()
		}
		if setRecordedPollVote(state, voterID, update.GetVote().GetSelectedOptions(), timestamp) {
			changed = true
		}
	}
	text, senderID := "", state.SenderID
	fromMe, timeSent, isRead := state.FromMe, state.TimeSent, state.IsRead
	if changed {
		savePollStates(pollStorePath(paths[handler.connId]), pollStates[handler.connId])
		text = formatPollState(state)
	}
	mx.Unlock()
	if changed {
		CWmNewMessagesNotify(handler.connId, chatID, info.ID, senderID, text,
			BoolToInt(fromMe), "", "", "", FileStatusNone, timeSent, BoolToInt(isRead), BoolToInt(true))
	}
}
