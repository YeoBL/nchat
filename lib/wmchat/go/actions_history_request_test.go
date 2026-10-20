package main

import (
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
