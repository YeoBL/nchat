package main

import (
	"fmt"
	"strings"
	"testing"

	"go.mau.fi/whatsmeow/types"
)

func TestChannelMetadataFailureKeepsSafeBrowsing(t *testing.T) {
	const connID = -193
	AddContactName(connID, "123@newsletter", "Known channel")
	AddContactName(connID, "456@g.us", "Not a channel")
	t.Cleanup(func() { mx.Lock(); delete(contacts, connID); mx.Unlock() })
	page := channelListPage(connID, nil, fmt.Errorf("argo decoding is currently broken"), "789@newsletter")
	seen := map[string]bool{}
	for _, item := range page.Items {
		if item.Operation == "channel_follow" || item.Operation == "channel_mute" || item.Operation == "channel_unfollow" {
			t.Fatalf("unavailable subscription control offered: %s", item.Operation)
		}
		if item.Operation == "channel_messages" && item.Params["channelId"] != "" {
			if value, resetsPage := item.Params["before"]; !resetsPage || value != "" {
				t.Fatal("new channel inherited an older channel's page position")
			}
			seen[item.Params["channelId"]] = true
			if !strings.Contains(item.Detail, "follow status unavailable") {
				t.Fatalf("cached channel presented as followed: %+v", item)
			}
		}
		if item.Operation == "channel_messages" && len(item.Fields) == 1 && item.Fields[0].Key == "channelId" {
			seen["directID"] = true
		}
		seen[item.Operation] = true
	}
	if !page.Success || !seen["123@newsletter"] || !seen["789@newsletter"] || !seen["directID"] || !seen["channel_list"] || seen["456@g.us"] || !strings.Contains(page.Text, "Use WhatsApp") {
		t.Fatalf("safe channel recovery missing: %+v", page)
	}
	page = channelListPage(connID, nil, nil, "")
	for _, item := range page.Items {
		if item.Operation == "channel_follow" {
			return
		}
	}
	t.Fatal("working metadata path lost channel-follow control")
}

func TestChannelPostsRemainBrowsableWithoutMetadata(t *testing.T) {
	jid := types.NewJID("123", types.NewsletterServer)
	meta := &types.NewsletterMetadata{ViewerMeta: &types.NewsletterViewerMetadata{Role: types.NewsletterRoleSubscriber}}
	for _, input := range []struct {
		meta *types.NewsletterMetadata
		err  error
	}{
		{meta, fmt.Errorf("argo decoding is currently broken")},
		{nil, nil},
		{&types.NewsletterMetadata{}, nil},
		{&types.NewsletterMetadata{ViewerMeta: &types.NewsletterViewerMetadata{Role: types.NewsletterRoleGuest}}, nil},
	} {
		page := channelPostsPage(jid, "Synthetic channel", "", 50, 42, input.meta, input.err)
		seen := map[string]bool{}
		for _, item := range page.Items {
			seen[item.Operation] = true
		}
		if !seen["open_chat"] || !seen["channel_messages"] || seen["channel_mute"] || seen["channel_unfollow"] {
			t.Fatalf("incorrect controls without verified subscription: %+v", page)
		}
	}
	page := channelPostsPage(jid, "Synthetic channel", "", 1, 0, meta, nil)
	seen := map[string]bool{}
	for _, item := range page.Items {
		seen[item.Operation] = true
		if item.Operation == "channel_mute" && !item.Confirm {
			t.Fatal("notification mutation did not require review")
		}
	}
	if !seen["channel_mute"] || !seen["channel_unfollow"] {
		t.Fatal("verified subscription lost controls")
	}
}

func TestChannelPostFailureOffersLocalHistoryAndRetry(t *testing.T) {
	jid := types.NewJID("123", types.NewsletterServer)
	for _, known := range []bool{false, true} {
		page := channelPostsError(jid, "Synthetic channel", fmt.Errorf("offline"), known)
		seen := map[string]bool{}
		for _, item := range page.Items {
			seen[item.Operation] = true
			if item.Params["chatId"] != "" && item.Params["chatId"] != jid.String() {
				t.Fatal("local history targeted another channel")
			}
		}
		if page.Success || seen["open_chat"] != known || !seen["channel_messages"] || !strings.Contains(page.Text, "Use WhatsApp") {
			t.Fatalf("post failure recovery missing: %+v", page)
		}
	}
	page := channelSubscriptionError("Channel notifications", fmt.Errorf("argo decoding is currently broken"))
	if page.Success || !strings.Contains(page.Text, "Check WhatsApp before retrying") || len(page.Items) != 1 || page.Items[0].Operation != "channel_list" {
		t.Fatalf("unconfirmed subscription change lacks safe recovery: %+v", page)
	}
}
