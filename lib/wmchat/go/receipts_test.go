package main

import (
	"context"
	"encoding/json"
	"errors"
	"testing"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waCommon"
	"go.mau.fi/whatsmeow/proto/waWeb"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"google.golang.org/protobuf/proto"
)

type receiptLIDStore struct {
	store.NoopStore
	pn  types.JID
	err error
}

func (s *receiptLIDStore) GetPNForLID(context.Context, types.JID) (types.JID, error) {
	return s.pn, s.err
}

func TestReceiptCanonicalUsers(t *testing.T) {
	self := types.NewJID("447700900100", types.DefaultUserServer)
	pn := types.NewJID("447700900123", types.DefaultUserServer)
	lid := types.NewJID("12345", types.HiddenUserServer)
	aliases := &receiptLIDStore{pn: pn}
	client := &whatsmeow.Client{Store: &store.Device{ID: &self, LIDs: aliases}}
	if got := canonicalReceiptUserID(client, lid.String()); got != pn.String() {
		t.Fatalf("verified LID mapping missing: %s", got)
	}
	if got := canonicalReceiptUserID(client, "447700900123:12@s.whatsapp.net"); got != pn.String() {
		t.Fatalf("device address duplicated recipient: %s", got)
	}
	aliases.err = errors.New("unavailable")
	if got := canonicalReceiptUserID(client, lid.String()); got != lid.String() {
		t.Fatalf("failed mapping invented recipient: %s", got)
	}
	if got := canonicalReceiptUserID(nil, lid.String()); got != lid.String() {
		t.Fatalf("offline mapping lost recipient: %s", got)
	}
	const connID = -126
	clients[connID] = client
	t.Cleanup(func() { delete(clients, connID) })
	aliases.err = nil
	var result map[string]string
	if json.Unmarshal([]byte(WmCanonicalReceiptUsers(connID, `["12345@lid","447700900123:12@s.whatsapp.net"]`)), &result) != nil ||
		result[lid.String()] != pn.String() || result["447700900123:12@s.whatsapp.net"] != pn.String() {
		t.Fatalf("canonical receipt bridge failed: %v", result)
	}
	if got := WmCanonicalReceiptUsers(connID, `{"bad":"input"}`); got != "{}" {
		t.Fatalf("invalid bridge input accepted: %s", got)
	}
}

func TestHistoricalReceipts(t *testing.T) {
	self := types.NewJID("447700900100", types.DefaultUserServer)
	pn := types.NewJID("447700900123", types.DefaultUserServer)
	client := &whatsmeow.Client{Store: &store.Device{ID: &self, LIDs: &receiptLIDStore{pn: pn}}}
	message := &waWeb.WebMessageInfo{
		Key: &waCommon.MessageKey{ID: proto.String("sent"), FromMe: proto.Bool(true)},
		UserReceipt: []*waWeb.UserReceipt{
			{UserJID: proto.String("12345@lid"), ReceiptTimestamp: proto.Int64(100), ReadTimestamp: proto.Int64(200)},
			{UserJID: proto.String(pn.String()), PlayedTimestamp: proto.Int64(300)},
			{UserJID: proto.String(self.String()), ReadTimestamp: proto.Int64(100)},
			{UserJID: proto.String("123@g.us"), ReadTimestamp: proto.Int64(100)},
			{UserJID: proto.String("unknown@lid"), ReceiptTimestamp: proto.Int64(-100)},
			{UserJID: proto.String(""), ReadTimestamp: proto.Int64(100)},
		},
	}
	got := historicalReceipts(client, message)
	if len(got) != 2 || got[0] != (recordedReceipt{pn.String(), 100, 200}) ||
		got[1] != (recordedReceipt{pn.String(), 0, 300}) {
		t.Fatalf("historical delivery/read/played import = %+v", got)
	}
	// A played confirmation implies delivery but cannot manufacture its date.
	if got[1].Delivered != 0 {
		t.Fatal("invented delivery timestamp")
	}
	message.Key.FromMe = proto.Bool(false)
	if len(historicalReceipts(client, message)) != 0 || len(historicalReceipts(client, nil)) != 0 {
		t.Fatal("incoming or absent history created recipient receipts")
	}
}

func TestHistoricalNotificationContext(t *testing.T) {
	var readStates []int
	handler := &WmEventHandler{connId: -127, notifyMessage: func(connID int, chatID, messageID, senderID, text string,
		fromMe int, quotedID, fileID, filePath string, fileStatus, timeSent, isRead, isEdited int) {
		if connID != -127 || chatID != "chat" || messageID != "message" || text != "text" {
			t.Fatal("notification routing changed")
		}
		readStates = append(readStates, isRead)
	}}
	// A normal recovered placeholder can be a fresh unread message; source web
	// metadata alone must not suppress it. Only an explicit old-history context does.
	handler.NotifyNewMessage(-127, "chat", "message", "sender", "text", 0, "", "", "", 0, 100, 0, 0)
	importer := *handler
	importer.importedHistory = true
	importer.NotifyNewMessage(-127, "chat", "message", "sender", "text", 0, "", "", "", 0, 100, 0, 0)
	handler.NotifyNewMessage(-127, "chat", "message", "sender", "text", 0, "", "", "", 0, 100, 1, 0)
	if len(readStates) != 3 || readStates[0] != 0 || readStates[1] != -1 || readStates[2] != 1 {
		t.Fatalf("history context escaped into live notifications: %v", readStates)
	}
}
