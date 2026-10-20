package main

import (
	"context"
	"crypto/sha256"
	"fmt"
	"slices"
	"sort"
	"strings"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waCommon"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/proto/waWeb"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
)

func canonicalPollVoterID(client *whatsmeow.Client, raw string) string {
	jid, err := types.ParseJID(raw)
	if err != nil || jid.User == "" {
		return raw
	}
	jid = jid.ToNonAD()
	raw = jid.String()
	if client == nil || client.Store == nil {
		return raw
	}
	if IsSelfUser(client, raw) && client.Store.ID != nil {
		return StrFromJid(*client.Store.ID)
	}
	if jid.Server == types.HiddenUserServer && client.Store.LIDs != nil {
		if phone, err := client.Store.LIDs.GetPNForLID(context.Background(), jid); err == nil && !phone.IsEmpty() {
			return phone.ToNonAD().String()
		}
	}
	return raw
}

// A phone number and its linked-device identity represent the same voter.
func normalizeRecordedPollVoters(state *PollState, canonical func(string) string) bool {
	if state == nil || len(state.Votes) == 0 {
		return false
	}
	ids := make([]string, 0, len(state.Votes))
	for id := range state.Votes {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	changed := false
	for _, id := range ids {
		target := canonical(id)
		if target == "" {
			target = id
		}
		if mergeRecordedPollHashes(state, target, state.Votes[id], state.VoteTimes[id]) {
			changed = true
		}
		if target != id {
			delete(state.Votes, id)
			delete(state.VoteTimes, id)
			changed = true
		}
	}
	return changed
}

// The phone may resend old snapshots after newer live votes, including withdrawals.
func setRecordedPollVote(state *PollState, voter string, selected [][]byte, timestamp int64) bool {
	hashes := make([]string, 0, len(selected))
	for _, option := range selected {
		hashes = append(hashes, fmt.Sprintf("%x", option))
	}
	return mergeRecordedPollHashes(state, voter, hashes, timestamp)
}

func mergeRecordedPollHashes(state *PollState, voter string, hashes []string, timestamp int64) bool {
	if state == nil || voter == "" || timestamp < 0 {
		return false
	}
	previous, exists := state.Votes[voter]
	previousTime, hasTime := state.VoteTimes[voter]
	if (exists || hasTime) && timestamp < previousTime {
		return false
	}
	hashes = slices.Clone(hashes)
	sort.Strings(hashes)
	hashes = slices.Compact(hashes)
	if exists && timestamp == previousTime {
		// Equal-time snapshots/aliases have no trustworthy ordering. Keep only
		// agreed choices, so even a same-timestamp withdrawal cannot be revived.
		common := hashes[:0]
		for _, hash := range hashes {
			if slices.Contains(previous, hash) {
				common = append(common, hash)
			}
		}
		hashes = common
		if hasTime && slices.Equal(previous, hashes) {
			return false
		}
	}
	if state.Votes == nil {
		state.Votes = make(map[string][]string)
	}
	if state.VoteTimes == nil {
		state.VoteTimes = make(map[string]int64)
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
		if self.User != "" {
			return self.ToNonAD()
		}
		return types.EmptyJID
	}
	if key.GetParticipant() != "" {
		participant := key.GetParticipant()
		voter, err := types.ParseJID(participant)
		if err == nil && strings.Count(participant, "@") == 1 && voter.User != "" &&
			(voter.Server == types.DefaultUserServer || voter.Server == types.HiddenUserServer) {
			return voter.ToNonAD()
		}
		return types.EmptyJID
	}
	if chat.User != "" && (chat.Server == types.DefaultUserServer || chat.Server == types.HiddenUserServer) {
		return chat.ToNonAD()
	}
	return types.EmptyJID
}

// PollAdditionalMetadata contains name-edit hashes, not voters. Only the
// decrypted choices actually supplied in PollUpdates can extend this ledger.
func historicalPollUpdates(chat, self types.JID, pollID string, updates []*waWeb.PollUpdate,
	canonical func(string) string) []pendingPollVote {
	if pollID == "" {
		return nil
	}
	var recovered []pendingPollVote
	for _, update := range updates {
		if update.GetVote() == nil || proto.Size(update) > maxPendingPollVoteSize {
			continue
		}
		voter := historicalPollVoter(chat, self, update)
		if voter.IsEmpty() {
			continue
		}
		voter, err := types.ParseJID(canonical(voter.String()))
		if err != nil || voter.User == "" ||
			(voter.Server != types.DefaultUserServer && voter.Server != types.HiddenUserServer) {
			continue
		}
		valid := true
		for _, hash := range update.GetVote().GetSelectedOptions() {
			if len(hash) != sha256.Size {
				valid = false
				break
			}
		}
		if !valid {
			continue // Do not turn corrupt/unsupported hashes into a withdrawal.
		}
		timestamp := update.GetSenderTimestampMS()
		if timestamp <= 0 {
			timestamp = update.GetServerTimestampMS()
		}
		if timestamp < 0 {
			continue
		}
		id := update.GetPollUpdateMessageKey().GetID()
		if len(id) > 256 {
			continue
		}
		if id == "" {
			hashes := make([]string, 0, len(update.GetVote().GetSelectedOptions()))
			for _, hash := range update.GetVote().GetSelectedOptions() {
				hashes = append(hashes, fmt.Sprintf("%x", hash))
			}
			sort.Strings(hashes)
			payload := fmt.Sprintf("%s\x00%d\x00%s", voter.String(), timestamp, strings.Join(slices.Compact(hashes), "\x00"))
			id = fmt.Sprintf("history-%x", sha256.Sum256([]byte(payload)))
		}
		recovered = append(recovered, pendingPollVote{
			key: pollStateKey(chat.String(), pollID),
			event: &events.Message{
				Info: types.MessageInfo{ID: id, MessageSource: types.MessageSource{Chat: chat, Sender: voter,
					IsGroup: chat.Server == types.GroupServer, IsFromMe: voter == self.ToNonAD()},
					Timestamp: time.UnixMilli(timestamp)},
				Message: &waE2E.Message{PollUpdateMessage: &waE2E.PollUpdateMessage{
					PollCreationMessageKey: &waCommon.MessageKey{ID: proto.String(pollID)},
					SenderTimestampMS:      proto.Int64(timestamp),
				}},
			},
			vote: update.GetVote(),
		})
	}
	return recovered
}

// History sync carries already-decrypted poll votes on the original poll message.
// It is supplementary history from the phone, not a complete server voter list.
func (handler *WmEventHandler) RecoverHistoricalPollVotes(info types.MessageInfo, message *waWeb.WebMessageInfo) {
	if len(message.GetPollUpdates()) == 0 {
		return
	}
	client := GetClient(handler.connId)
	if client == nil || client.Store == nil || client.Store.ID == nil {
		return
	}
	chatID := GetChatId(client, &info.Chat, &info.Sender)
	updates := historicalPollUpdates(info.Chat, *client.Store.ID, info.ID, message.GetPollUpdates(),
		func(raw string) string { return canonicalPollVoterID(client, raw) })
	mx.Lock()
	state := pollStates[handler.connId][pollStateKey(chatID, info.ID)]
	if state == nil {
		for _, update := range updates {
			queuePendingPollVote(handler.connId, pollStateKey(chatID, info.ID), update.event, update.vote)
		}
		if len(updates) > 0 {
			savePollCache(handler.connId)
		}
		mx.Unlock()
		return
	}
	changed := normalizeRecordedPollVoters(state, func(raw string) string { return canonicalPollVoterID(client, raw) })
	for _, update := range updates {
		if recordPollUpdate(state, client, update) {
			changed = true
		}
	}
	text, senderID := "", state.SenderID
	fromMe, timeSent, isRead := state.FromMe, state.TimeSent, state.IsRead
	if changed {
		savePollCache(handler.connId)
		text = formatPollState(state)
	}
	mx.Unlock()
	if changed {
		handler.NotifyNewMessage(handler.connId, chatID, info.ID, senderID, text,
			BoolToInt(fromMe), "", "", "", FileStatusNone, timeSent, BoolToInt(isRead), BoolToInt(true))
	}
}
