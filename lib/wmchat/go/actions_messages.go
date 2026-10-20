package main

import (
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"math"
	"mime"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protoreflect"
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
	viewOnce := false
	for depth := 0; msg != nil && depth < 12; depth++ {
		var inner *waE2E.Message
		switch {
		case msg.GetEphemeralMessage() != nil:
			inner = msg.GetEphemeralMessage().GetMessage()
		case msg.GetViewOnceMessage() != nil:
			inner = msg.GetViewOnceMessage().GetMessage()
			viewOnce = true
		case msg.GetViewOnceMessageV2() != nil:
			inner = msg.GetViewOnceMessageV2().GetMessage()
			viewOnce = true
		case msg.GetViewOnceMessageV2Extension() != nil:
			inner = msg.GetViewOnceMessageV2Extension().GetMessage()
			viewOnce = true
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
	if viewOnce || msg.GetImageMessage().GetViewOnce() || msg.GetVideoMessage().GetViewOnce() {
		msg = markDeliveredViewOnce(msg)
	}
	return msg
}

func markDeliveredViewOnce(msg *waE2E.Message) *waE2E.Message {
	if msg == nil {
		return nil
	}
	copy := proto.Clone(msg).(*waE2E.Message)
	if image := copy.GetImageMessage(); image != nil {
		image.ViewOnce = proto.Bool(true)
		if !strings.HasPrefix(image.GetCaption(), "[View once]") {
			image.Caption = proto.String(strings.TrimSpace("[View once] " + image.GetCaption()))
		}
	}
	if video := copy.GetVideoMessage(); video != nil {
		video.ViewOnce = proto.Bool(true)
		if !strings.HasPrefix(video.GetCaption(), "[View once]") {
			video.Caption = proto.String(strings.TrimSpace("[View once] " + video.GetCaption()))
		}
	}
	if audio := copy.GetAudioMessage(); audio != nil {
		audio.ViewOnce = proto.Bool(true)
	}
	if videoNote := copy.GetPtvMessage(); videoNote != nil {
		videoNote.ViewOnce = proto.Bool(true)
	}
	return copy
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
	case msg.GetButtonsMessage() != nil:
		buttons := msg.GetButtonsMessage()
		lines := []string{buttons.GetContentText()}
		for _, button := range buttons.GetButtons() {
			lines = append(lines, "• "+button.GetButtonText().GetDisplayText())
		}
		lines = append(lines, "[Open Account tools → Message details to respond]")
		return strings.Join(lines, "\n")
	case msg.GetListMessage() != nil:
		list := msg.GetListMessage()
		lines := []string{list.GetTitle(), list.GetDescription()}
		for _, section := range list.GetSections() {
			for _, row := range section.GetRows() {
				lines = append(lines, "• "+row.GetTitle()+" — "+row.GetDescription())
			}
		}
		lines = append(lines, "[Open Account tools → Message details to choose]")
		return strings.Join(lines, "\n")
	case msg.GetButtonsResponseMessage() != nil:
		return "[Button response] " + msg.GetButtonsResponseMessage().GetSelectedDisplayText()
	case msg.GetListResponseMessage() != nil:
		return "[List response] " + msg.GetListResponseMessage().GetTitle()
	case msg.GetInteractiveMessage() != nil:
		interactive := msg.GetInteractiveMessage()
		text := strings.TrimSpace("[Interactive message]\n" + interactive.GetHeader().GetTitle() + "\n" + interactive.GetBody().GetText() + "\n" + interactive.GetFooter().GetText())
		for _, button := range nativeActionButtons(msg) {
			text += "\n• " + button.Text
		}
		return text
	case msg.GetInteractiveResponseMessage() != nil:
		return "[Interactive response] " + msg.GetInteractiveResponseMessage().GetBody().GetText()
	}
	return ""
}

func wmMessageAction(ctx context.Context, connID int, cli *whatsmeow.Client, op string, p map[string]string) (actionResult, bool) {
	if op == "forward_messages" {
		return forwardMessagesAction(ctx, connID, cli, p), true
	}
	if op == "send_recording" {
		return sendRecordingAction(ctx, connID, cli, p), true
	}
	if op != "message_details" && op != "business_reply" {
		return actionResult{}, false
	}
	entry, msg := messageActionEntry(connID, p["chatId"], p["msgId"])
	if msg == nil {
		return actionError("Message details", fmt.Errorf("structured details are not loaded for this message; load earlier phone history or wait for it to sync")), true
	}
	if op == "business_reply" {
		chat, err := actionChat(p)
		if err != nil {
			return actionError("Reply", err), true
		}
		ci := &waE2E.ContextInfo{StanzaID: proto.String(entry.ID), Participant: proto.String(entry.SenderID), QuotedMessage: msg}
		var reply *waE2E.Message
		if buttons := msg.GetButtonsMessage(); buttons != nil {
			for _, button := range buttons.GetButtons() {
				if button.GetButtonID() == p["buttonId"] && button.GetButtonText() != nil {
					reply = &waE2E.Message{ButtonsResponseMessage: &waE2E.ButtonsResponseMessage{SelectedButtonID: proto.String(button.GetButtonID()), Response: &waE2E.ButtonsResponseMessage_SelectedDisplayText{SelectedDisplayText: button.GetButtonText().GetDisplayText()}, Type: waE2E.ButtonsResponseMessage_DISPLAY_TEXT.Enum(), ContextInfo: ci}}
					break
				}
			}
		} else if list := msg.GetListMessage(); list != nil {
			for _, section := range list.GetSections() {
				for _, row := range section.GetRows() {
					if row.GetRowID() == p["rowId"] {
						reply = &waE2E.Message{ListResponseMessage: &waE2E.ListResponseMessage{Title: proto.String(row.GetTitle()), Description: proto.String(row.GetDescription()), ListType: waE2E.ListResponseMessage_SINGLE_SELECT.Enum(), SingleSelectReply: &waE2E.ListResponseMessage_SingleSelectReply{SelectedRowID: proto.String(row.GetRowID())}, ContextInfo: ci}}
					}
				}
			}
		}
		if reply == nil {
			for _, button := range nativeActionButtons(msg) {
				if button.Name == "quick_reply" && button.ID == p["nativeButtonId"] {
					reply = nativeQuickReply(button, ci)
					break
				}
			}
		}
		if reply == nil {
			return actionError("Business reply", fmt.Errorf("this button is unavailable or requires the official app")), true
		}
		return sendActionMessage(ctx, connID, cli, chat, reply, "Reply sent"), true
	}
	r := actionOK("Message details", structuredMessageText(msg))
	if r.Text == "" {
		r.Text = "This message has no additional structured controls."
	}
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
	if buttons := msg.GetButtonsMessage(); buttons != nil {
		for _, button := range buttons.GetButtons() {
			if button.GetButtonText() != nil {
				r.Items = append(r.Items, action(button.GetButtonText().GetDisplayText(), "Send this button reply", "business_reply", map[string]string{"buttonId": button.GetButtonID()}, nil, true))
			}
		}
	}
	if list := msg.GetListMessage(); list != nil {
		for _, section := range list.GetSections() {
			for _, row := range section.GetRows() {
				r.Items = append(r.Items, action(row.GetTitle(), row.GetDescription(), "business_reply", map[string]string{"rowId": row.GetRowID()}, nil, true))
			}
		}
	}
	if msg.GetInteractiveMessage() != nil {
		buttons := nativeActionButtons(msg)
		for _, button := range buttons {
			if button.Name == "quick_reply" {
				r.Items = append(r.Items, action(button.Text, "Send this button reply", "business_reply", map[string]string{"nativeButtonId": button.ID}, nil, true))
			} else if button.Name == "cta_url" {
				r.Items = append(r.Items, action(button.Text, button.URL, "openUrl", map[string]string{"url": button.URL}, nil, false))
			}
		}
		if len(buttons) == 0 {
			r.Text += "\nThis interactive form requires the official app."
		}
	}
	return r, true
}

type nativeActionButton struct{ Name, ID, Text, URL string }

func nativeActionButtons(msg *waE2E.Message) []nativeActionButton {
	buttons := []nativeActionButton{}
	for _, button := range msg.GetInteractiveMessage().GetNativeFlowMessage().GetButtons() {
		var values map[string]string
		if json.Unmarshal([]byte(button.GetButtonParamsJSON()), &values) != nil {
			continue
		}
		item := nativeActionButton{Name: button.GetName(), ID: values["id"], Text: values["display_text"], URL: values["url"]}
		if item.Text == "" {
			continue
		}
		if item.Name == "quick_reply" && item.ID != "" {
			buttons = append(buttons, item)
		} else if item.Name == "cta_url" {
			parsed, err := url.Parse(item.URL)
			if err == nil && parsed.Host != "" && (parsed.Scheme == "https" || parsed.Scheme == "http") {
				buttons = append(buttons, item)
			}
		}
	}
	return buttons
}

func nativeQuickReply(button nativeActionButton, ci *waE2E.ContextInfo) *waE2E.Message {
	params, _ := json.Marshal(map[string]string{"id": button.ID, "display_text": button.Text})
	return &waE2E.Message{InteractiveResponseMessage: &waE2E.InteractiveResponseMessage{Body: &waE2E.InteractiveResponseMessage_Body{Text: proto.String(button.Text), Format: waE2E.InteractiveResponseMessage_Body_DEFAULT.Enum()}, ContextInfo: ci, InteractiveResponseMessage: &waE2E.InteractiveResponseMessage_NativeFlowResponseMessage_{NativeFlowResponseMessage: &waE2E.InteractiveResponseMessage_NativeFlowResponseMessage{Name: proto.String("quick_reply"), ParamsJSON: proto.String(string(params)), Version: proto.Int32(3)}}}}
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

func sendRecordingAction(ctx context.Context, connID int, cli *whatsmeow.Client, p map[string]string) actionResult {
	chat, err := actionChat(p)
	if err != nil {
		return actionError("Send recording", err)
	}
	path := p["path"]
	if !filepath.IsAbs(path) {
		return actionError("Send recording", fmt.Errorf("recording path must be absolute"))
	}
	stat, err := os.Stat(path)
	if err != nil {
		return actionError("Send recording", err)
	}
	if !stat.Mode().IsRegular() || stat.Size() == 0 || stat.Size() > 100*1024*1024 {
		return actionError("Send recording", fmt.Errorf("recording must be a regular file smaller than 100 MB"))
	}
	seconds, err := recordingSeconds(p["durationSec"])
	if err != nil {
		return actionError("Send recording", fmt.Errorf("recording duration is unavailable"))
	}
	mimeType, _, err := mime.ParseMediaType(p["mimeType"])
	if err != nil {
		return actionError("Send recording", err)
	}
	kind := p["mediaKind"]
	mediaType := whatsmeow.MediaAudio
	if kind == "voice" && mimeType != "audio/ogg" {
		return actionError("Send recording", fmt.Errorf("voice notes must be Ogg Opus audio"))
	}
	if kind == "video" {
		if mimeType != "video/mp4" {
			return actionError("Send recording", fmt.Errorf("video notes must be MP4"))
		}
		mediaType = whatsmeow.MediaVideo
	} else if kind != "voice" {
		return actionError("Send recording", fmt.Errorf("choose a voice or video note"))
	}
	waveform, decodeErr := base64.StdEncoding.DecodeString(p["waveform"])
	if kind == "voice" && (decodeErr != nil || len(waveform) > 256) {
		return actionError("Voice note", fmt.Errorf("invalid waveform"))
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return actionError("Send recording", err)
	}
	upload, err := cli.Upload(ctx, data, mediaType)
	if err != nil {
		return actionError("Send recording", err)
	}
	var msg *waE2E.Message
	if kind == "voice" {
		msg = &waE2E.Message{AudioMessage: &waE2E.AudioMessage{URL: proto.String(upload.URL), DirectPath: proto.String(upload.DirectPath), MediaKey: upload.MediaKey, FileEncSHA256: upload.FileEncSHA256, FileSHA256: upload.FileSHA256, FileLength: proto.Uint64(uint64(len(data))), Mimetype: proto.String("audio/ogg; codecs=opus"), PTT: proto.Bool(true), Seconds: proto.Uint32(uint32(seconds)), Waveform: waveform}}
	} else {
		msg = &waE2E.Message{PtvMessage: &waE2E.VideoMessage{URL: proto.String(upload.URL), DirectPath: proto.String(upload.DirectPath), MediaKey: upload.MediaKey, FileEncSHA256: upload.FileEncSHA256, FileSHA256: upload.FileSHA256, FileLength: proto.Uint64(uint64(len(data))), Mimetype: proto.String("video/mp4"), Seconds: proto.Uint32(uint32(seconds))}}
	}
	return sendActionMessage(ctx, connID, cli, chat, msg, "Recording sent")
}

func recordingSeconds(raw string) (uint32, error) {
	duration, err := strconv.ParseFloat(raw, 64)
	if err != nil || math.IsNaN(duration) || math.IsInf(duration, 0) || duration <= 0 || duration > 86400 {
		return 0, fmt.Errorf("invalid recording duration")
	}
	return uint32(math.Ceil(duration)), nil
}

func forwardableMessage(msg *waE2E.Message) bool {
	return msg.Conversation != nil || msg.ExtendedTextMessage != nil || msg.ImageMessage != nil || msg.VideoMessage != nil || msg.AudioMessage != nil || msg.PtvMessage != nil || msg.DocumentMessage != nil || msg.StickerMessage != nil || msg.ContactMessage != nil || msg.ContactsArrayMessage != nil || msg.LocationMessage != nil || msg.LiveLocationMessage != nil || msg.PollCreationMessage != nil || msg.PollCreationMessageV2 != nil || msg.PollCreationMessageV3 != nil || msg.ButtonsMessage != nil || msg.ListMessage != nil || msg.InteractiveMessage != nil
}

func forwardedMessage(original *waE2E.Message) (*waE2E.Message, error) {
	if !forwardableMessage(original) {
		return nil, fmt.Errorf("this message type cannot be forwarded")
	}
	if original.GetImageMessage().GetViewOnce() || original.GetVideoMessage().GetViewOnce() || original.GetAudioMessage().GetViewOnce() || original.GetPtvMessage().GetViewOnce() {
		return nil, fmt.Errorf("view-once messages cannot be forwarded")
	}
	msg := proto.Clone(original).(*waE2E.Message)
	if msg.Conversation != nil {
		msg = &waE2E.Message{ExtendedTextMessage: &waE2E.ExtendedTextMessage{Text: msg.Conversation}}
	}
	root := msg.ProtoReflect()
	root.Range(func(field protoreflect.FieldDescriptor, value protoreflect.Value) bool {
		if field.Kind() != protoreflect.MessageKind || field.IsList() || field.IsMap() {
			return true
		}
		child := value.Message()
		contextField := child.Descriptor().Fields().ByName("contextInfo")
		if contextField == nil {
			return true
		}
		ci := &waE2E.ContextInfo{}
		if child.Has(contextField) {
			if existing, ok := child.Get(contextField).Message().Interface().(*waE2E.ContextInfo); ok {
				ci = proto.Clone(existing).(*waE2E.ContextInfo)
			}
		}
		ci.StanzaID = nil
		ci.Participant = nil
		ci.QuotedMessage = nil
		ci.RemoteJID = nil
		ci.ForwardingScore = proto.Uint32(ci.GetForwardingScore() + 1)
		ci.IsForwarded = proto.Bool(true)
		child.Set(contextField, protoreflect.ValueOfMessage(ci.ProtoReflect()))
		return true
	})
	return msg, nil
}

func forwardMessagesAction(ctx context.Context, connID int, cli *whatsmeow.Client, p map[string]string) actionResult {
	chat, err := actionChat(p)
	if err != nil {
		return actionError("Forward messages", err)
	}
	if chat.Server == types.NewsletterServer || chat.Server == types.BroadcastServer {
		return actionError("Forward messages", fmt.Errorf("choose a direct or group destination"))
	}
	ids := strings.Fields(p["messageIds"])
	if len(ids) == 0 || len(ids) > 100 {
		return actionError("Forward messages", fmt.Errorf("select 1–100 messages"))
	}
	messages := make([]*waE2E.Message, 0, len(ids))
	for _, id := range ids {
		_, original := messageActionEntry(connID, p["sourceChatId"], id)
		if original == nil {
			return actionError("Forward messages", fmt.Errorf("message %s has no native payload yet; load its phone history before forwarding", id))
		}
		msg, err := forwardedMessage(original)
		if err != nil {
			return actionError("Forward messages", err)
		}
		poll := original.GetPollCreationMessage()
		if poll == nil {
			poll = original.GetPollCreationMessageV2()
		}
		if poll == nil {
			poll = original.GetPollCreationMessageV3()
		}
		if poll != nil {
			options := []string{}
			for _, option := range poll.GetOptions() {
				options = append(options, option.GetOptionName())
			}
			msg = cli.BuildPollCreation(poll.GetName(), options, int(poll.GetSelectableOptionsCount()))
		}
		messages = append(messages, msg)
	}
	for index, msg := range messages {
		r := sendActionMessage(ctx, connID, cli, chat, msg, "Forward messages")
		if !r.Success {
			r.Text = fmt.Sprintf("Forwarded %d of %d messages. %s", index, len(messages), r.Text)
			return r
		}
	}
	return actionOK("Messages forwarded", fmt.Sprintf("Forwarded %d messages.", len(messages)))
}
