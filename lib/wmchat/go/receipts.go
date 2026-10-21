package main

import (
	"context"
	"encoding/json"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waHistorySync"
	"go.mau.fi/whatsmeow/proto/waWeb"
	"go.mau.fi/whatsmeow/types"
)

func suppliedHistoryCounts(history *waHistorySync.HistorySync) (messages, receipts, votes int) {
	for _, conversation := range history.GetConversations() {
		for _, entry := range conversation.GetMessages() {
			message := entry.GetMessage()
			if message == nil {
				continue
			}
			messages++
			receipts += len(message.GetUserReceipt())
			votes += len(message.GetPollUpdates())
		}
	}
	return
}

// Only WhatsApp's own linked-identity mapping is authoritative. Contact names
// and matching display numbers are not enough to merge two receipt recipients.
func canonicalReceiptUserID(client *whatsmeow.Client, raw string) string {
	jid, err := types.ParseJID(raw)
	if err != nil || jid.IsEmpty() {
		return raw
	}
	jid = jid.ToNonAD()
	if jid.Server == types.HiddenUserServer && client != nil && client.Store != nil && client.Store.LIDs != nil {
		if pn, err := client.Store.LIDs.GetPNForLID(context.Background(), jid); err == nil && !pn.IsEmpty() {
			jid = pn.ToNonAD()
		}
	}
	return jid.String()
}

func WmCanonicalReceiptUsers(connID int, encoded string) string {
	var users []string
	if json.Unmarshal([]byte(encoded), &users) != nil || len(users) > 100000 {
		return "{}"
	}
	client := GetClient(connID)
	aliases := make(map[string]string, len(users))
	for _, user := range users {
		aliases[user] = canonicalReceiptUserID(client, user)
	}
	result, _ := json.Marshal(aliases)
	return string(result)
}

type recordedReceipt struct {
	UserID          string
	Delivered, Read int64
}

type messageNotifyFunc func(connID int, chatID, messageID, senderID, text string,
	fromMe int, quotedID, fileID, filePath string, fileStatus, timeSent, isRead, isEdited int)

func (handler *WmEventHandler) NotifyNewMessage(connID int, chatID, messageID, senderID, text string,
	fromMe int, quotedID, fileID, filePath string, fileStatus, timeSent, isRead, isEdited int) {
	if handler.importedHistory {
		isRead = -1 // The C++ cache preserves known read state; this never sends a receipt.
	}
	notify := handler.notifyMessage
	if notify == nil {
		notify = CWmNewMessagesNotify
	}
	notify(connID, chatID, messageID, senderID, text, fromMe, quotedID, fileID, filePath,
		fileStatus, timeSent, isRead, isEdited)
}

func historicalReceipts(client *whatsmeow.Client, message *waWeb.WebMessageInfo) []recordedReceipt {
	if message == nil || !message.GetKey().GetFromMe() || message.GetKey().GetID() == "" {
		return nil
	}
	var receipts []recordedReceipt
	for _, receipt := range message.GetUserReceipt() {
		jid, err := types.ParseJID(receipt.GetUserJID())
		if err != nil || jid.IsEmpty() ||
			(jid.Server != types.DefaultUserServer && jid.Server != types.HiddenUserServer) {
			continue
		}
		userID := canonicalReceiptUserID(client, jid.String())
		if client != nil && client.Store != nil && IsSelfUser(client, userID) {
			continue
		}
		delivered, read := receipt.GetReceiptTimestamp(), receipt.GetReadTimestamp()
		if delivered < 0 {
			delivered = 0
		}
		if read <= 0 {
			read = receipt.GetPlayedTimestamp()
		}
		if read < 0 {
			read = 0
		}
		if delivered > 0 || read > 0 {
			receipts = append(receipts, recordedReceipt{userID, delivered, read})
		}
	}
	return receipts
}

func (handler *WmEventHandler) RecoverHistoricalReceipts(chat types.JID, message *waWeb.WebMessageInfo) {
	client := GetClient(handler.connId)
	if client == nil || client.Store == nil {
		return
	}
	for _, receipt := range historicalReceipts(client, message) {
		CWmMessageReceipt(handler.connId, GetChatId(client, &chat, nil), message.GetKey().GetID(),
			receipt.UserID, receipt.Delivered, receipt.Read)
	}
}
