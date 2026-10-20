package main

import (
	"context"
	"strconv"
	"testing"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/types"
)

func TestEarlierHistoryRequestUsesMilliseconds(t *testing.T) {
	chat := types.NewJID("123", types.GroupServer)
	stamp := time.Date(2026, time.January, 1, 12, 0, 0, 255000000, time.UTC)
	params := map[string]string{"timeSent": strconv.FormatInt(stamp.UnixMilli(), 10), "msgId": "synthetic-anchor", "isOutgoing": "1"}
	message, err := buildEarlierHistoryRequest(&whatsmeow.Client{}, chat, params)
	if err != nil {
		t.Fatal(err)
	}
	request := message.GetProtocolMessage().GetPeerDataOperationRequestMessage().GetHistorySyncOnDemandRequest()
	if request.GetOldestMsgTimestampMS() != stamp.Unix() || request.GetOldestMsgID() != params["msgId"] ||
		request.GetChatJID() != chat.String() || !request.GetOldestMsgFromMe() || request.GetOnDemandMsgCount() != 50 {
		t.Fatalf("incorrect phone-history request: %v", request)
	}
	for _, invalid := range []string{"", "invalid", "0", "-1", "999", "9223372036854775808"} {
		params["timeSent"] = invalid
		if _, err := buildEarlierHistoryRequest(&whatsmeow.Client{}, chat, params); err == nil {
			t.Fatalf("invalid history timestamp accepted: %q", invalid)
		}
	}
	params["timeSent"] = strconv.FormatInt(stamp.UnixMilli(), 10)
	params["msgId"] = ""
	if _, err := buildEarlierHistoryRequest(&whatsmeow.Client{}, chat, params); err == nil {
		t.Fatal("missing history anchor accepted")
	}
}

func TestSelectedHistoryRecoveryRequiresLaterAnchor(t *testing.T) {
	chat := types.NewJID("123", types.GroupServer)
	params := map[string]string{"msgId": "later-anchor", "timeSent": "60001", "targetMsgId": "selected", "targetTimeSent": "59999"}
	message, err := buildEarlierHistoryRequest(&whatsmeow.Client{}, chat, params)
	if err != nil || message == nil {
		t.Fatalf("valid selected history request failed: %v", err)
	}
	for _, invalid := range []string{"", "invalid", "60000", "60099", "61000", "-1"} {
		params["targetTimeSent"] = invalid
		if _, err := buildEarlierHistoryRequest(&whatsmeow.Client{}, chat, params); err == nil {
			t.Fatalf("invalid target accepted: %q", invalid)
		}
	}
	params["targetTimeSent"] = "59999"
	params["targetMsgId"] = params["msgId"]
	if _, err := buildEarlierHistoryRequest(&whatsmeow.Client{}, chat, params); err == nil {
		t.Fatal("target itself accepted as history anchor")
	}
	params["targetMsgId"] = "selected"
	for _, synthetic := range []string{"60", "+local"} {
		params["msgId"] = synthetic
		if _, err := buildEarlierHistoryRequest(&whatsmeow.Client{}, chat, params); err == nil {
			t.Fatalf("local synthetic anchor accepted: %s", synthetic)
		}
	}
	params["msgId"] = "later-anchor"
	for _, server := range []string{types.NewsletterServer, types.BroadcastServer, "invalid"} {
		if _, err := buildEarlierHistoryRequest(&whatsmeow.Client{}, types.NewJID("123", server), params); err == nil {
			t.Fatalf("unsupported chat accepted: %s", server)
		}
	}
}

func TestSelectedHistoryRecoveryIsExplicitAndConfirmed(t *testing.T) {
	page := wmClientAction(context.Background(), -1, "menu", map[string]string{"chatId": "123@g.us", "msgId": "selected"})
	found := false
	for _, item := range page.Items {
		if item.Operation == "local.whatsapp.history" {
			found = true
			if !item.Confirm || len(item.Fields) != 0 {
				t.Fatal("history recovery must be an explicit confirmed action without a blocking form")
			}
		}
	}
	if !found {
		t.Fatal("selected message history recovery missing from account tools")
	}
}
