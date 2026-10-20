package main

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"google.golang.org/protobuf/proto"
)

func TestActionForms(t *testing.T) {
	if seconds, err := recordingSeconds("1.234000"); err != nil || seconds != 2 {
		t.Fatalf("native recorder duration rejected: %d %v", seconds, err)
	}
	for _, duration := range []string{"NaN", "Inf", "0", "-1", "86401"} {
		if _, err := recordingSeconds(duration); err == nil {
			t.Fatalf("invalid duration accepted: %s", duration)
		}
	}
	if options, limit, err := validateNewPoll("Lunch?", "Tea | Coffee", "2"); err != nil || len(options) != 2 || limit != 2 {
		t.Fatalf("poll rejected: %v %d %v", options, limit, err)
	}
	for _, input := range []struct{ question, options, limit string }{{"", "A|B", "1"}, {"Q", "A|A", "1"}, {"Q", "A|", "1"}, {"Q", "A|B", "3"}, {"Q", "A", "0"}} {
		if _, _, err := validateNewPoll(input.question, input.options, input.limit); err == nil {
			t.Fatalf("invalid poll accepted: %+v", input)
		}
	}
	for _, input := range [][2]string{{"NaN", "0"}, {"Inf", "1"}, {"91", "0"}, {"0", "181"}} {
		if _, _, err := validateLocation(input[0], input[1]); err == nil {
			t.Fatalf("invalid coordinate accepted: %v", input)
		}
	}
	if _, _, err := validateLocation("52.205", "0.119"); err != nil {
		t.Fatal(err)
	}
	if !validPhoneNumber("+44 7700 900123") || validPhoneNumber("123\r\nFN:Injected") {
		t.Fatal("contact phone validation failed")
	}
	if _, err := channelJID("123@newsletter", types.NewsletterServer); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"", "123@g.us", "123@newsletter@newsletter", "@newsletter"} {
		if _, err := channelJID(id, types.NewsletterServer); err == nil {
			t.Fatalf("invalid channel accepted: %q", id)
		}
	}
	var menu actionResult
	if err := json.Unmarshal([]byte(WmClientAction(-99, "menu", `{"chatId":"123@g.us","msgId":"message"}`)), &menu); err != nil || !menu.Success {
		t.Fatalf("menu JSON failed: %+v %v", menu, err)
	}
	seen := map[string]bool{}
	for _, item := range menu.Items {
		seen[item.Operation] = true
	}
	for _, op := range []string{"status_list", "channel_list", "community_list", "create_poll", "group_info", "message_details", "history_request"} {
		if !seen[op] {
			t.Fatalf("missing menu entry %s", op)
		}
	}
}

func TestStructuredMessagesAndForwarding(t *testing.T) {
	native := &waE2E.Message{InteractiveMessage: &waE2E.InteractiveMessage{InteractiveMessage: &waE2E.InteractiveMessage_NativeFlowMessage_{NativeFlowMessage: &waE2E.InteractiveMessage_NativeFlowMessage{Buttons: []*waE2E.InteractiveMessage_NativeFlowMessage_NativeFlowButton{{Name: proto.String("quick_reply"), ButtonParamsJSON: proto.String(`{"id":"yes","display_text":"Yes"}`)}, {Name: proto.String("cta_url"), ButtonParamsJSON: proto.String(`{"url":"file:///private/file","display_text":"Bad link"}`)}}}}}}
	buttons := nativeActionButtons(native)
	if len(buttons) != 1 || buttons[0].ID != "yes" {
		t.Fatalf("native button validation failed: %+v", buttons)
	}
	reply := nativeQuickReply(buttons[0], nil).GetInteractiveResponseMessage()
	if reply.GetBody().GetText() != "Yes" || !strings.Contains(reply.GetNativeFlowResponseMessage().GetParamsJSON(), `"id":"yes"`) {
		t.Fatal("native quick reply lost its ID")
	}
	card := &waE2E.ContactMessage{DisplayName: proto.String("Sam"), Vcard: proto.String("BEGIN:VCARD\r\nFN:Sam\r\nTEL;TYPE=CELL:+447700900123\r\nEND:VCARD")}
	if text := structuredMessageText(&waE2E.Message{ContactMessage: card}); !strings.Contains(text, "Sam") || !strings.Contains(text, "+447700900123") {
		t.Fatalf("contact not shown: %s", text)
	}
	if items := contactActions(card); len(items) != 1 || items[0].Params["chatId"] != "447700900123@s.whatsapp.net" {
		t.Fatalf("contact navigation failed: %+v", items)
	}
	image := &waE2E.Message{ImageMessage: &waE2E.ImageMessage{Caption: proto.String("Photo"), DirectPath: proto.String("/own-delivered-media"), MediaKey: []byte{1}}}
	wrapped := &waE2E.Message{ViewOnceMessageV2: &waE2E.FutureProofMessage{Message: image}}
	unwrapped := unwrapActionMessage(wrapped)
	if !unwrapped.GetImageMessage().GetViewOnce() || unwrapped.GetImageMessage().GetDirectPath() != image.GetImageMessage().GetDirectPath() {
		t.Fatal("delivered view-once payload lost")
	}
	if image.GetImageMessage().GetViewOnce() {
		t.Fatal("original payload mutated")
	}
	if _, err := forwardedMessage(unwrapped); err == nil {
		t.Fatal("view-once forwarding accepted")
	}
	onceAudio := unwrapActionMessage(&waE2E.Message{ViewOnceMessageV2: &waE2E.FutureProofMessage{Message: &waE2E.Message{AudioMessage: &waE2E.AudioMessage{PTT: proto.Bool(true)}}}})
	if !onceAudio.GetAudioMessage().GetViewOnce() {
		t.Fatal("wrapper-only voice note lost view-once restriction")
	}
	if _, err := forwardedMessage(onceAudio); err == nil {
		t.Fatal("view-once voice note forwarding accepted")
	}
	text := &waE2E.Message{Conversation: proto.String("Hello")}
	forward, err := forwardedMessage(text)
	if err != nil || forward.GetExtendedTextMessage().GetText() != "Hello" || !forward.GetExtendedTextMessage().GetContextInfo().GetIsForwarded() {
		t.Fatalf("native forwarding failed: %v %v", forward, err)
	}
	if text.GetConversation() != "Hello" || text.GetExtendedTextMessage() != nil {
		t.Fatal("original message changed while forwarding")
	}
}

func TestActionPayloadPersistence(t *testing.T) {
	const connID = -192
	path := t.TempDir()
	paths[connID] = path
	t.Cleanup(func() { closeActionMessages(path); delete(paths, connID) })
	chat, _ := types.ParseJID("123@g.us")
	sender, _ := types.ParseJID("447700900123@s.whatsapp.net")
	msg := &waE2E.Message{Conversation: proto.String("Forward me")}
	recordActionMessage(connID, types.MessageInfo{MessageSource: types.MessageSource{Chat: chat, Sender: sender}, ID: "message"}, msg)
	closeActionMessages(path)
	entry, restored := messageActionEntry(connID, chat.String(), "message")
	if restored == nil || restored.GetConversation() != "Forward me" || entry.SenderID != sender.String() {
		t.Fatalf("payload not restored: %+v %v", entry, restored)
	}
}

func TestStatusExpiryAndAudience(t *testing.T) {
	now := time.Unix(1000000, 0)
	values := map[string]storedStatus{"expired": {Timestamp: now.Add(-24 * time.Hour).Unix()}, "fresh": {Timestamp: now.Add(-23 * time.Hour).Unix()}}
	if !pruneStatuses(values, now) || len(values) != 1 || values["fresh"].Timestamp == 0 {
		t.Fatal("status expiry boundary failed")
	}
	if statusKey("sender-a", "same") == statusKey("sender-b", "same") {
		t.Fatal("status sender collision")
	}
	a, _ := types.ParseJID("1@s.whatsapp.net")
	b, _ := types.ParseJID("2@s.whatsapp.net")
	first := types.StatusPrivacy{Type: types.StatusPrivacyTypeWhitelist, List: []types.JID{a, b}}
	second := types.StatusPrivacy{Type: types.StatusPrivacyTypeWhitelist, List: []types.JID{b, a}}
	if statusAudienceToken(first) != statusAudienceToken(second) {
		t.Fatal("audience order changed token")
	}
	second.Type = types.StatusPrivacyTypeBlacklist
	if statusAudienceToken(first) == statusAudienceToken(second) {
		t.Fatal("different status audiences share token")
	}
	if got := statusSummary(&waE2E.Message{ImageMessage: &waE2E.ImageMessage{Caption: proto.String("A caption")}}); got != "Photo A caption" {
		t.Fatalf("status caption missing: %s", got)
	}
}
