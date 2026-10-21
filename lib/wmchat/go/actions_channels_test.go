package main

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"go.mau.fi/whatsmeow"
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
	if !strings.Contains(page.Text, "No followed channels") || !strings.Contains(page.Text, "without following") {
		t.Fatal("empty followed-channel list did not explain read-only browsing")
	}
	readOnlyLinkForm := false
	for _, item := range page.Items {
		if item.Operation == "channel_messages" && len(item.Fields) == 1 {
			readOnlyLinkForm = !item.Confirm && item.Params["_quickForm"] == "1" &&
				strings.Contains(item.Title, "link or ID") && strings.Contains(item.Fields[0].Label, "link or ID")
		}
	}
	if !readOnlyLinkForm {
		t.Fatal("read-only channel link form requires an unnecessary review or hides supported input")
	}
	for _, item := range page.Items {
		if item.Operation == "channel_follow" && item.Confirm {
			return
		}
	}
	t.Fatal("working metadata path lost confirmed channel-follow control")
}

func TestChannelLinkResolutionIsReadOnly(t *testing.T) {
	jid := types.NewJID("123", types.NewsletterServer)
	meta := &types.NewsletterMetadata{ID: jid}
	const canonical = "https://whatsapp.com/channel/SyntheticInvite"
	for _, value := range []string{
		canonical, " " + canonical + " ", canonical + "/", canonical + "?tracking=1#details",
		"https://www.whatsapp.com/channel/SyntheticInvite/?tracking=1#details",
	} {
		calls := 0
		got, gotMeta, err := resolveChannelInput(context.Background(), value, func(ctx context.Context, link string) (*types.NewsletterMetadata, error) {
			calls++
			if link != canonical {
				t.Fatalf("public metadata lookup got unnormalized link: %s", link)
			}
			return meta, nil
		})
		if err != nil || got != jid || gotMeta != meta || calls != 1 {
			t.Fatalf("public link resolution failed for %q: %s, %v", value, got, err)
		}
	}
	got, gotMeta, err := resolveChannelInput(context.Background(), " 123@newsletter ", func(context.Context, string) (*types.NewsletterMetadata, error) {
		t.Fatal("raw ID unnecessarily resolved through an invite")
		return nil, nil
	})
	if err != nil || got != jid || gotMeta != nil {
		t.Fatalf("raw channel-ID browsing changed: %s, %v", got, err)
	}
	for _, value := range []string{
		"https://example.com/channel/SyntheticInvite", "http://whatsapp.com/channel/SyntheticInvite",
		"https://whatsapp.com/channel/", "https://whatsapp.com/channel/one/two",
		"https://user@whatsapp.com/channel/SyntheticInvite", "https://whatsapp.com:8443/channel/SyntheticInvite", "123@g.us",
	} {
		if _, _, err := resolveChannelInput(context.Background(), value, func(context.Context, string) (*types.NewsletterMetadata, error) {
			t.Fatalf("invalid channel input reached lookup: %s", value)
			return nil, nil
		}); err == nil {
			t.Fatalf("invalid channel input accepted: %s", value)
		}
	}
	for _, result := range []struct {
		meta *types.NewsletterMetadata
		err  error
	}{{nil, nil}, {&types.NewsletterMetadata{ID: types.NewJID("123", types.GroupServer)}, nil}, {nil, context.DeadlineExceeded}} {
		if _, _, err := resolveChannelInput(context.Background(), canonical, func(context.Context, string) (*types.NewsletterMetadata, error) {
			return result.meta, result.err
		}); err == nil {
			t.Fatal("missing, invalid or failed channel metadata was accepted")
		}
	}
}

func TestChannelPostsRemainBrowsableWithoutMetadata(t *testing.T) {
	jid := types.NewJID("123", types.NewsletterServer)
	meta := &types.NewsletterMetadata{ViewerMeta: &types.NewsletterViewerMetadata{Role: types.NewsletterRoleSubscriber, Mute: types.NewsletterMuteOff}}
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

func TestChannelHistoryContextPreservesReadState(t *testing.T) {
	var states []int
	live := &WmEventHandler{connId: -194, notifyMessage: func(connID int, chatID, messageID, senderID, text string,
		fromMe int, quotedID, fileID, filePath string, fileStatus, sent, read, edited int) {
		if connID != -194 || chatID != "123@newsletter" || messageID != "post" {
			t.Fatal("channel import changed message routing")
		}
		states = append(states, read)
	}}
	importer := channelHistoryImporter(live)
	importer.NotifyNewMessage(-194, "123@newsletter", "post", "123@newsletter", "post text", 0, "", "", "", 0, 100, 1, 0)
	live.NotifyNewMessage(-194, "123@newsletter", "post", "123@newsletter", "post text", 0, "", "", "", 0, 100, 0, 0)
	if live.importedHistory || len(states) != 2 || states[0] != -1 || states[1] != 0 {
		t.Fatalf("history import changed existing read state or live handler context: %v", states)
	}
}

func TestChannelPaginationUsesRawAdvancingServerIDs(t *testing.T) {
	messages := make([]*types.NewsletterMessage, 50)
	for index := range messages {
		// A legitimate full page of deleted/unsupported entries has no payloads.
		messages[index] = &types.NewsletterMessage{MessageServerID: 200 - index}
	}
	params := &whatsmeow.GetNewsletterMessagesParams{Count: 50}
	if got := channelPostsCursor(messages, params); got != 151 {
		t.Fatalf("contentless full page lost older history: %d", got)
	}
	params.Before = 151
	if got := channelPostsCursor(messages, params); got != 0 {
		t.Fatalf("non-advancing service page repeats forever: %d", got)
	}
	messages[0].MessageServerID = 149
	if got := channelPostsCursor(messages, params); got != 149 {
		t.Fatalf("advancing raw server ID was not used: %d", got)
	}
	messages[0], messages[1].MessageServerID = nil, 0
	if got := channelPostsCursor(messages, params); got != 0 {
		t.Fatalf("invalid server IDs invented a cursor: %d", got)
	}
	params.Before = 0
	if got := channelPostsCursor(messages[:49], params); got != 0 {
		t.Fatalf("short final page offered older history: %d", got)
	}
}

func TestChannelMetadataDoesNotInventMuteState(t *testing.T) {
	jid := types.NewJID("123", types.NewsletterServer)
	meta := &types.NewsletterMetadata{ViewerMeta: &types.NewsletterViewerMetadata{Role: types.NewsletterRoleSubscriber}}
	for _, state := range []types.NewsletterMuteState{"", "future", types.NewsletterMuteOn, types.NewsletterMuteOff} {
		meta.ViewerMeta.Mute = state
		muted, known := channelMuteState(meta, nil)
		wantKnown := state == types.NewsletterMuteOn || state == types.NewsletterMuteOff
		if known != wantKnown || muted != (state == types.NewsletterMuteOn) {
			t.Fatalf("mute metadata %q = %t,%t", state, muted, known)
		}
		page := channelPostsPage(jid, "Synthetic channel", "", 1, 0, meta, nil)
		found := false
		for _, item := range page.Items {
			if item.Operation == "channel_mute" {
				found = true
				if !item.Confirm || item.Params["muted"] != fmt.Sprint(!muted) {
					t.Fatal("mute control did not reflect confirmed state")
				}
			}
		}
		if found != wantKnown {
			t.Fatalf("mute control invented from unknown metadata %q", state)
		}
	}
	for _, input := range []struct {
		meta *types.NewsletterMetadata
		err  error
	}{
		{nil, nil}, {&types.NewsletterMetadata{}, nil}, {meta, errors.New("partial metadata")},
		{&types.NewsletterMetadata{ViewerMeta: &types.NewsletterViewerMetadata{Role: types.NewsletterRoleGuest, Mute: types.NewsletterMuteOff}}, nil},
	} {
		if _, known := channelMuteState(input.meta, input.err); known {
			t.Fatal("unavailable metadata changed existing mute state")
		}
	}
}

func TestChannelPostingPermissionIsBoundedAndExplained(t *testing.T) {
	jid := types.NewJID("123", types.NewsletterServer)
	for _, role := range []types.NewsletterRole{"", types.NewsletterRoleGuest, types.NewsletterRoleSubscriber, types.NewsletterRoleAdmin, types.NewsletterRoleOwner} {
		meta := &types.NewsletterMetadata{ViewerMeta: &types.NewsletterViewerMetadata{Role: role}}
		lookup := func(ctx context.Context, got types.JID) (*types.NewsletterMetadata, error) {
			deadline, bounded := ctx.Deadline()
			if got != jid || !bounded || time.Until(deadline) <= 0 || time.Until(deadline) > 8*time.Second {
				t.Fatal("posting permission lookup is unbounded or changed target")
			}
			return meta, nil
		}
		err := verifyChannelPosting(context.Background(), jid, "", lookup)
		allowed := role == types.NewsletterRoleAdmin || role == types.NewsletterRoleOwner
		if (err == nil) != allowed {
			t.Fatalf("posting permission for role %q = %v", role, err)
		}
		page := channelPostsPage(jid, "Synthetic channel", "", 1, 0, meta, nil)
		if allowed && !strings.Contains(page.Text, "normal chat composer publishes") {
			t.Fatal("administrator page hides existing publishing functionality")
		}
		if !allowed && !strings.Contains(page.Text, "cannot reply") {
			t.Fatal("subscriber page omits publishing restriction")
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	err := verifyChannelPosting(ctx, jid, "", func(ctx context.Context, _ types.JID) (*types.NewsletterMetadata, error) {
		return nil, ctx.Err()
	})
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("posting verification lost cancellation: %v", err)
	}
	err = verifyChannelPosting(context.Background(), jid, "synthetic-image.png", func(context.Context, types.JID) (*types.NewsletterMetadata, error) {
		t.Fatal("unsupported channel attachment reached permission lookup or upload")
		return nil, nil
	})
	if err == nil || !strings.Contains(err.Error(), "attachment publishing is not supported") {
		t.Fatalf("broken channel media transport was not blocked: %v", err)
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
