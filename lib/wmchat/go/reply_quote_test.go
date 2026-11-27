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
