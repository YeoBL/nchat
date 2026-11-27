package main

import (
	"go.mau.fi/whatsmeow/proto/waE2E"
	"google.golang.org/protobuf/proto"
	"testing"
)

func TestReplyQuoteSnapshot(t *testing.T) {
	if replyQuoteText(nil) != "" {
		t.Fatal("nil message")
	}
	q := &waE2E.Message{Conversation: proto.String("custom 🙂 quote")}
	ci := &waE2E.ContextInfo{QuotedMessage: q}
	for _, message := range []*waE2E.Message{
		{ExtendedTextMessage: &waE2E.ExtendedTextMessage{ContextInfo: ci}},
		{ImageMessage: &waE2E.ImageMessage{ContextInfo: ci}},
		{VideoMessage: &waE2E.VideoMessage{ContextInfo: ci}},
		{AudioMessage: &waE2E.AudioMessage{ContextInfo: ci}},
		{DocumentMessage: &waE2E.DocumentMessage{ContextInfo: ci}},
	} {
		if replyQuoteText(message) != "custom 🙂 quote" {
			t.Fatal("lost quote snapshot")
		}
	}
}

func TestOutgoingReplyQuoteWireSnapshot(t *testing.T) {
	context := replyContext("real-source-id", "Changed quote Ω", "sender@c.us")
	message := &waE2E.Message{ExtendedTextMessage: &waE2E.ExtendedTextMessage{Text: proto.String("Actual reply"), ContextInfo: &context}}
	wire, err := proto.Marshal(message)
	if err != nil {
		t.Fatal(err)
	}
	var decoded waE2E.Message
	if err = proto.Unmarshal(wire, &decoded); err != nil {
		t.Fatal(err)
	}
	got := decoded.GetExtendedTextMessage().GetContextInfo()
	if got.GetStanzaID() != "real-source-id" || got.GetQuotedMessage().GetConversation() != "Changed quote Ω" || got.GetParticipant() != "sender@s.whatsapp.net" {
		t.Fatal("quote changed during wire encoding")
	}
}
