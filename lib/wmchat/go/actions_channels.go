package main

import (
	"context"
	"crypto/sha256"
	"fmt"
	"slices"
	"sort"
	"strconv"
	"strings"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/types"
)

func channelJID(value, server string) (types.JID, error) {
	value = strings.TrimSpace(value)
	jid, err := types.ParseJID(value)
	if err != nil || jid.User == "" || jid.Server != server || strings.Count(value, "@") != 1 {
		label := "channel"
		if server == types.GroupServer {
			label = "community"
		}
		return types.EmptyJID, fmt.Errorf("enter a valid %s ID", label)
	}
	return jid, nil
}

func wmChannelAction(ctx context.Context, connId int, cli *whatsmeow.Client, op string, p map[string]string) (actionResult, bool) {
	switch op {
	case "channel_list":
		channels, err := cli.GetSubscribedNewsletters(ctx)
		channels = slices.DeleteFunc(channels, func(channel *types.NewsletterMetadata) bool { return channel == nil })
		sort.SliceStable(channels, func(i, j int) bool {
			return strings.ToLower(channels[i].ThreadMeta.Name.Text) < strings.ToLower(channels[j].ThreadMeta.Name.Text)
		})
		result := actionOK("WhatsApp channels", "Choose a channel to load its latest posts. Follow and mute changes sync with WhatsApp.")
		if err != nil {
			result.Text = "WhatsApp could not return the followed-channel list: " + err.Error() + ". You can still try opening a channel by its ID."
		}
		for _, channel := range channels {
			name := channel.ThreadMeta.Name.Text
			if strings.TrimSpace(name) == "" {
				name = channel.ID.String()
			}
			detail := fmt.Sprintf("%d subscribers", channel.ThreadMeta.SubscriberCount)
			if channel.ViewerMeta != nil && channel.ViewerMeta.Mute == types.NewsletterMuteOn {
				detail += " · muted"
			}
			result.Items = append(result.Items, action(name, detail, "channel_messages",
				map[string]string{"channelId": channel.ID.String()}, nil, false))
		}
		result.Items = append(result.Items, action("Follow a channel", "Paste its WhatsApp channel link or channel ID", "channel_follow", nil,
			[]actionField{{Key: "id", Label: "Channel link or ID"}}, true))
		result.Items = append(result.Items, action("Open channel by ID", "For example, 123456789@newsletter", "channel_messages", nil,
			[]actionField{{Key: "channelId", Label: "Channel ID"}}, false))
		return result, true

	case "channel_messages", "channel_mute", "channel_unfollow":
		value := p["channelId"]
		if value == "" {
			value = p["id"]
		}
		jid, err := channelJID(value, types.NewsletterServer)
		if err != nil {
			return actionError("WhatsApp channel", err), true
		}
		if op == "channel_mute" {
			muted := p["muted"] != "false"
			if err := cli.NewsletterToggleMute(ctx, jid, muted); err != nil {
				return actionError("Channel notifications", err), true
			}
			CWmUpdateMuteNotify(connId, jid.String(), BoolToInt(muted))
			if muted {
				return actionOK("Channel muted", "Channel notification setting updated in WhatsApp."), true
			}
			return actionOK("Channel unmuted", "Channel notification setting updated in WhatsApp."), true
		}
		if op == "channel_unfollow" {
			if err := cli.UnfollowNewsletter(ctx, jid); err != nil {
				return actionError("Unfollow channel", err), true
			}
			CWmUpdateMuteNotify(connId, jid.String(), 1)
			return actionOK("Channel unfollowed", "You no longer follow this channel. Previously downloaded posts remain as muted local history."), true
		}
		meta, metaErr := cli.GetNewsletterInfo(ctx, jid)
		params := &whatsmeow.GetNewsletterMessagesParams{Count: 50}
		if p["before"] != "" {
			params.Before, err = strconv.Atoi(p["before"])
			if err != nil || params.Before <= 0 {
				return actionError("WhatsApp channel", fmt.Errorf("invalid message position")), true
			}
		}
		messages, err := cli.GetNewsletterMessages(ctx, jid, params)
		if err != nil {
			return actionError("Channel posts", err), true
		}
		messages = slices.DeleteFunc(messages, func(message *types.NewsletterMessage) bool { return message == nil })
		handler := GetHandler(connId)
		if handler == nil {
			return actionError("Channel posts", fmt.Errorf("WhatsApp connection is no longer available")), true
		}
		name, description := GetContactName(connId, jid.String()), ""
		if meta != nil {
			if strings.TrimSpace(meta.ThreadMeta.Name.Text) != "" {
				name = meta.ThreadMeta.Name.Text
			}
			description = meta.ThreadMeta.Description.Text
		}
		CWmNewContactsNotify(connId, jid.String(), name, "", 0, 0, NotifyDirect)
		AddContactName(connId, jid.String(), name)
		sort.SliceStable(messages, func(i, j int) bool { return messages[i].MessageServerID < messages[j].MessageServerID })
		first, latest, count := 0, 0, 0
		for _, message := range messages {
			if message.Message == nil {
				continue
			}
			id := message.MessageID
			if id == "" {
				id = fmt.Sprintf("newsletter-%x-%d", sha256.Sum256([]byte(jid.String())), message.MessageServerID)
			}
			info := types.MessageInfo{
				MessageSource: types.MessageSource{Chat: jid, Sender: jid},
				ID:            id, ServerID: message.MessageServerID, Timestamp: message.Timestamp, PushName: name,
			}
			// Explicit history fetches do not create unread notifications or report channel views.
			handler.HandleMessage(info, message.Message, true)
			if first == 0 || message.MessageServerID < first {
				first = message.MessageServerID
			}
			if int(message.Timestamp.Unix()) > latest {
				latest = int(message.Timestamp.Unix())
			}
			count++
		}
		muted := meta != nil && meta.ViewerMeta != nil && meta.ViewerMeta.Mute == types.NewsletterMuteOn
		if count > 0 && meta != nil && meta.ViewerMeta != nil && metaErr == nil {
			CWmNewChatsNotify(connId, jid.String(), 0, BoolToInt(muted), 0, 0, latest)
		}
		result := actionOK(name, fmt.Sprintf("%s\n\nLoaded %d posts into the channel chat. Posting and channel administration are not available here.", description, count))
		if metaErr != nil {
			result.Text += "\nChannel details and subscription controls are unavailable: " + metaErr.Error()
		}
		if count > 0 {
			result.Items = append(result.Items, action("Open channel posts", "Read posts and open attachments", "open_chat", map[string]string{"chatId": jid.String()}, nil, false))
		}
		if len(messages) >= params.Count && first > 0 {
			result.Items = append(result.Items, action("Load older posts", "Fetch the previous 50 posts", "channel_messages",
				map[string]string{"channelId": jid.String(), "before": strconv.Itoa(first)}, nil, false))
		}
		if meta != nil && metaErr == nil {
			muteTitle := "Mute channel"
			if muted {
				muteTitle = "Unmute channel"
			}
			result.Items = append(result.Items, action(muteTitle, "Update WhatsApp channel notifications", "channel_mute",
				map[string]string{"channelId": jid.String(), "muted": strconv.FormatBool(!muted)}, nil, false))
			result.Items = append(result.Items, action("Unfollow channel", "Stop following this channel on WhatsApp", "channel_unfollow",
				map[string]string{"channelId": jid.String()}, nil, true))
		}
		return result, true

	case "channel_follow":
		value := strings.TrimSpace(p["id"])
		var meta *types.NewsletterMetadata
		var err error
		if strings.HasPrefix(value, whatsmeow.NewsletterLinkPrefix) || strings.HasPrefix(value, "https://www.whatsapp.com/channel/") {
			value = strings.Replace(value, "https://www.whatsapp.com/channel/", whatsmeow.NewsletterLinkPrefix, 1)
			meta, err = cli.GetNewsletterInfoWithInvite(ctx, strings.SplitN(value, "?", 2)[0])
		} else {
			var jid types.JID
			jid, err = channelJID(value, types.NewsletterServer)
			if err == nil {
				meta, err = cli.GetNewsletterInfo(ctx, jid)
			}
		}
		if err != nil {
			return actionError("Follow channel", err), true
		}
		if meta == nil {
			return actionError("Follow channel", fmt.Errorf("channel not found")), true
		}
		if err := cli.FollowNewsletter(ctx, meta.ID); err != nil {
			return actionError("Follow channel", err), true
		}
		result := actionOK("Channel followed", meta.ThreadMeta.Name.Text)
		result.Items = append(result.Items, action("View channel", "Load its latest posts", "channel_messages", map[string]string{"channelId": meta.ID.String()}, nil, false))
		return result, true

	case "community_list":
		groups, err := cli.GetJoinedGroups(ctx)
		if err != nil {
			return actionError("WhatsApp communities", err), true
		}
		groups = slices.DeleteFunc(groups, func(group *types.GroupInfo) bool { return group == nil })
		sort.SliceStable(groups, func(i, j int) bool { return strings.ToLower(groups[i].Name) < strings.ToLower(groups[j].Name) })
		result := actionOK("WhatsApp communities", "Choose a community to browse its groups.")
		for _, group := range groups {
			if group.IsParent {
				result.Items = append(result.Items, action(group.Name, group.Topic, "community_groups", map[string]string{"communityId": group.JID.String()}, nil, false))
			}
		}
		if len(result.Items) == 0 {
			result.Text = "No joined communities were returned by WhatsApp. Ordinary joined groups remain in the chat list."
		}
		return result, true

	case "community_groups":
		jid, err := channelJID(p["communityId"], types.GroupServer)
		if err != nil {
			return actionError("Community groups", err), true
		}
		groups, err := cli.GetSubGroups(ctx, jid)
		if err != nil {
			return actionError("Community groups", err), true
		}
		groups = slices.DeleteFunc(groups, func(group *types.GroupLinkTarget) bool { return group == nil })
		joined, err := cli.GetJoinedGroups(ctx)
		if err != nil {
			return actionError("Community groups", err), true
		}
		membership := make(map[types.JID]bool, len(joined))
		for _, group := range joined {
			if group != nil {
				membership[group.JID] = true
			}
		}
		sort.SliceStable(groups, func(i, j int) bool { return strings.ToLower(groups[i].Name) < strings.ToLower(groups[j].Name) })
		result := actionOK("Community groups", "Open a joined group. Groups you have not joined are listed for reference; join requests and community administration are not available here.")
		for _, group := range groups {
			detail := "Joined group"
			if group.IsDefaultSubGroup {
				detail = "Announcement group"
			}
			operation := "open_chat"
			if !membership[group.JID] {
				detail = "Not joined"
				operation = ""
			} else {
				CWmNewContactsNotify(connId, group.JID.String(), group.Name, "", 0, 0, NotifyDirect)
				AddContactName(connId, group.JID.String(), group.Name)
			}
			result.Items = append(result.Items, action(group.Name, detail, operation, map[string]string{"chatId": group.JID.String()}, nil, false))
		}
		return result, true
	}
	return actionResult{}, false
}
