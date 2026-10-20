package main

import (
	"context"
	"database/sql"

	"fmt"

	"os"
	"path/filepath"

	"strings"
	"sync"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"google.golang.org/protobuf/proto"
)

type storedActionMessage struct {
	ChatID   string
	SenderID string
	ID       string
	Data     []byte
}

var actionMessageMu sync.Mutex
var actionMessageDBs = map[string]*sql.DB{}

// Caller holds actionMessageMu. These payloads preserve native forwarding and structured controls.
func actionMessagesDB(path string) (*sql.DB, error) {
	if db := actionMessageDBs[path]; db != nil {
		return db, nil
	}
	if path == "" {
		return nil, fmt.Errorf("account storage is unavailable")
	}
	db, err := sql.Open("sqlite3", filepath.Join(path, "message-payloads.db")+"?_journal_mode=WAL&_synchronous=NORMAL&_busy_timeout=5000")
	if err != nil {
		return nil, err
	}
	if _, err = db.Exec(`CREATE TABLE IF NOT EXISTS payloads (chat TEXT NOT NULL,id TEXT NOT NULL,sender TEXT NOT NULL,payload BLOB NOT NULL,PRIMARY KEY(chat,id))`); err != nil {
		db.Close()
		return nil, err
	}
	_ = os.Chmod(filepath.Join(path, "message-payloads.db"), 0600)
	actionMessageDBs[path] = db
	return db, nil
}

func closeActionMessages(path string) {
	actionMessageMu.Lock()
	defer actionMessageMu.Unlock()
	if db := actionMessageDBs[path]; db != nil {
		db.Close()
		delete(actionMessageDBs, path)
	}
}

func unwrapActionMessage(msg *waE2E.Message) *waE2E.Message {
	for depth := 0; msg != nil && depth < 12; depth++ {
		var inner *waE2E.Message
		switch {
		case msg.GetEphemeralMessage() != nil:
			inner = msg.GetEphemeralMessage().GetMessage()
		case msg.GetDocumentWithCaptionMessage() != nil:
			inner = msg.GetDocumentWithCaptionMessage().GetMessage()
		case msg.GetDeviceSentMessage() != nil:
			inner = msg.GetDeviceSentMessage().GetMessage()
		}
		if inner == nil {
			break
		}
		msg = inner
	}
	if msg == nil {
		return nil
	}
	return msg
}

func recordActionMessage(connID int, info types.MessageInfo, msg *waE2E.Message) {
	if msg == nil || info.ID == "" || !forwardableMessage(msg) {
		return
	}
	data, err := proto.Marshal(msg)
	if err != nil {
		return
	}
	client := GetClient(connID)
	entry := storedActionMessage{ChatID: GetChatId(client, &info.Chat, &info.Sender), SenderID: info.Sender.String(), ID: info.ID, Data: data}
	path := GetPath(connID)
	actionMessageMu.Lock()
	defer actionMessageMu.Unlock()
	db, err := actionMessagesDB(path)
	if err == nil {
		_, err = db.Exec(`INSERT INTO payloads(chat,id,sender,payload) VALUES(?,?,?,?) ON CONFLICT(chat,id) DO UPDATE SET sender=excluded.sender,payload=excluded.payload`, entry.ChatID, entry.ID, entry.SenderID, entry.Data)
	}
	if err != nil {
		LOG_WARNING(fmt.Sprintf("store structured message: %v", err))
	}
}

func messageActionEntry(connID int, chatID, msgID string) (storedActionMessage, *waE2E.Message) {
	path := GetPath(connID)
	actionMessageMu.Lock()
	entry := storedActionMessage{ChatID: chatID, ID: msgID}
	db, err := actionMessagesDB(path)
	if err == nil {
		err = db.QueryRow(`SELECT sender,payload FROM payloads WHERE chat=? AND id=?`, chatID, msgID).Scan(&entry.SenderID, &entry.Data)
	}
	actionMessageMu.Unlock()
	msg := &waE2E.Message{}
	if err != nil || proto.Unmarshal(entry.Data, msg) != nil || len(entry.Data) == 0 {
		return entry, nil
	}
	return entry, msg
}

func forwardableMessage(msg *waE2E.Message) bool {
	return msg.Conversation != nil || msg.ExtendedTextMessage != nil || msg.ImageMessage != nil || msg.VideoMessage != nil || msg.AudioMessage != nil || msg.PtvMessage != nil || msg.DocumentMessage != nil || msg.StickerMessage != nil || msg.ContactMessage != nil || msg.ContactsArrayMessage != nil || msg.LocationMessage != nil || msg.LiveLocationMessage != nil || msg.PollCreationMessage != nil || msg.PollCreationMessageV2 != nil || msg.PollCreationMessageV3 != nil || msg.ButtonsMessage != nil || msg.ListMessage != nil || msg.InteractiveMessage != nil
}

func contactCardText(contact *waE2E.ContactMessage) string {
	if contact == nil {
		return ""
	}
	lines := []string{"[Contact] " + contact.GetDisplayName()}
	for _, line := range strings.Split(strings.ReplaceAll(contact.GetVcard(), "\r\n", "\n"), "\n") {
		key, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		key = strings.ToUpper(strings.SplitN(key, ";", 2)[0])
		if key == "TEL" || key == "EMAIL" || key == "ORG" {
			lines = append(lines, value)
		}
	}
	return strings.Join(lines, "\n")
}

func structuredMessageText(msg *waE2E.Message) string {
	switch {
	case msg.GetContactMessage() != nil:
		return contactCardText(msg.GetContactMessage())
	case msg.GetContactsArrayMessage() != nil:
		parts := []string{}
		for _, contact := range msg.GetContactsArrayMessage().GetContacts() {
			parts = append(parts, contactCardText(contact))
		}
		return strings.Join(parts, "\n\n")
	case msg.GetLocationMessage() != nil:
		loc := msg.GetLocationMessage()
		return fmt.Sprintf("[Location] %s\n%s\nhttps://maps.apple.com/?ll=%g,%g", loc.GetName(), loc.GetAddress(), loc.GetDegreesLatitude(), loc.GetDegreesLongitude())
	case msg.GetLiveLocationMessage() != nil:
		loc := msg.GetLiveLocationMessage()
		return fmt.Sprintf("[Live location snapshot] %s\nhttps://maps.apple.com/?ll=%g,%g", loc.GetCaption(), loc.GetDegreesLatitude(), loc.GetDegreesLongitude())
	}
	return ""
}

func wmMessageAction(ctx context.Context, connID int, cli *whatsmeow.Client, op string, p map[string]string) (actionResult, bool) {
	if op != "message_details" {
		return actionResult{}, false
	}
	_, msg := messageActionEntry(connID, p["chatId"], p["msgId"])
	if msg == nil {
		return actionError("Message details", fmt.Errorf("structured details are not loaded for this message")), true
	}
	r := actionOK("Message details", structuredMessageText(msg))
	if loc := msg.GetLocationMessage(); loc != nil {
		r.Items = append(r.Items, action("Open map", loc.GetName(), "openUrl", map[string]string{"url": fmt.Sprintf("https://maps.apple.com/?ll=%g,%g", loc.GetDegreesLatitude(), loc.GetDegreesLongitude())}, nil, false))
	}
	if loc := msg.GetLiveLocationMessage(); loc != nil {
		r.Items = append(r.Items, action("Open location snapshot", "Live updates are not tracked here", "openUrl", map[string]string{"url": fmt.Sprintf("https://maps.apple.com/?ll=%g,%g", loc.GetDegreesLatitude(), loc.GetDegreesLongitude())}, nil, false))
	}
	if contact := msg.GetContactMessage(); contact != nil {
		r.Items = append(r.Items, contactActions(contact)...)
	}
	if cards := msg.GetContactsArrayMessage(); cards != nil {
		for _, contact := range cards.GetContacts() {
			r.Items = append(r.Items, contactActions(contact)...)
		}
	}
	return r, true
}

func contactActions(contact *waE2E.ContactMessage) []actionItem {
	items := []actionItem{}
	for _, line := range strings.Split(strings.ReplaceAll(contact.GetVcard(), "\r\n", "\n"), "\n") {
		key, phone, ok := strings.Cut(line, ":")
		if !ok || !strings.HasPrefix(strings.ToUpper(key), "TEL") || !validPhoneNumber(phone) {
			continue
		}
		digits := strings.Map(func(r rune) rune {
			if r >= '0' && r <= '9' {
				return r
			}
			return -1
		}, phone)
		items = append(items, action("Chat with "+contact.GetDisplayName(), phone, "open_chat", map[string]string{"chatId": digits + "@s.whatsapp.net"}, nil, false))
	}
	return items
}
