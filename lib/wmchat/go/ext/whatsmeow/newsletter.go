// Copyright (c) 2023 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package whatsmeow

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/beeper/argo-go/codec"
	"github.com/beeper/argo-go/pkg/buf"
	"github.com/elliotchance/orderedmap/v3"

	"go.mau.fi/whatsmeow/argo"
	waBinary "go.mau.fi/whatsmeow/binary"
	"go.mau.fi/whatsmeow/proto/waWa6"
	"go.mau.fi/whatsmeow/types"
)

// NewsletterSubscribeLiveUpdates subscribes to receive live updates from a WhatsApp channel temporarily (for the duration returned).
func (cli *Client) NewsletterSubscribeLiveUpdates(ctx context.Context, jid types.JID) (time.Duration, error) {
	resp, err := cli.sendIQ(ctx, infoQuery{
		Namespace: "newsletter",
		Type:      iqSet,
		To:        jid,
		Content: []waBinary.Node{{
			Tag: "live_updates",
		}},
	})
	if err != nil {
		return 0, err
	}
	child := resp.GetChildByTag("live_updates")
	dur := child.AttrGetter().Int("duration")
	return time.Duration(dur) * time.Second, nil
}

// NewsletterMarkViewed marks a channel message as viewed, incrementing the view counter.
//
// This is not the same as marking the channel as read on your other devices, use the usual MarkRead function for that.
func (cli *Client) NewsletterMarkViewed(ctx context.Context, jid types.JID, serverIDs []types.MessageServerID) error {
	if cli == nil {
		return ErrClientIsNil
	}
	items := make([]waBinary.Node, len(serverIDs))
	for i, id := range serverIDs {
		items[i] = waBinary.Node{
			Tag: "item",
			Attrs: waBinary.Attrs{
				"server_id": id,
			},
		}
	}
	reqID := cli.generateRequestID()
	resp := cli.waitResponse(reqID)
	err := cli.sendNode(ctx, waBinary.Node{
		Tag: "receipt",
		Attrs: waBinary.Attrs{
			"to":   jid,
			"type": "view",
			"id":   reqID,
		},
		Content: []waBinary.Node{{
			Tag:     "list",
			Content: items,
		}},
	})
	if err != nil {
		cli.cancelResponse(reqID, resp)
		return err
	}
	// TODO handle response?
	<-resp
	return nil
}

// NewsletterSendReaction sends a reaction to a channel message.
// To remove a reaction sent earlier, set reaction to an empty string.
//
// The last parameter is the message ID of the reaction itself. It can be left empty to let whatsmeow generate a random one.
func (cli *Client) NewsletterSendReaction(ctx context.Context, jid types.JID, serverID types.MessageServerID, reaction string, messageID types.MessageID) error {
	if messageID == "" {
		messageID = cli.GenerateMessageID()
	}
	reactionAttrs := waBinary.Attrs{}
	messageAttrs := waBinary.Attrs{
		"to":        jid,
		"id":        messageID,
		"server_id": serverID,
		"type":      "reaction",
	}
	if reaction != "" {
		reactionAttrs["code"] = reaction
	} else {
		messageAttrs["edit"] = string(types.EditAttributeSenderRevoke)
	}
	return cli.sendNode(ctx, waBinary.Node{
		Tag:   "message",
		Attrs: messageAttrs,
		Content: []waBinary.Node{{
			Tag:   "reaction",
			Attrs: reactionAttrs,
		}},
	})
}

const (
	queryFetchNewsletter           = "6563316087068696"
	queryFetchNewsletterDehydrated = "7272540469429201"
	queryRecommendedNewsletters    = "7263823273662354" // variables -> input -> {limit: 20, country_codes: [string]}, output: xwa2_newsletters_recommended
	queryNewslettersDirectory      = "6190824427689257" // variables -> input -> {view: "RECOMMENDED", limit: 50, start_cursor: base64, filters: {country_codes: [string]}}
	querySubscribedNewsletters     = "6388546374527196" // variables -> empty, output: xwa2_newsletter_subscribed
	queryNewsletterSubscribers     = "9800646650009898" // variables -> input -> {newsletter_id, count}, output: xwa2_newsletter_subscribers -> subscribers -> edges
	mutationMuteNewsletter         = "6274038279359549" // variables -> {newsletter_id, updates->{description, settings}}, output: xwa2_newsletter_update -> NewsletterMetadata without viewer meta
	mutationUnmuteNewsletter       = "6068417879924485"
	mutationUpdateNewsletter       = "7150902998257522"
	mutationCreateNewsletter       = "6234210096708695"
	mutationUnfollowNewsletter     = "6392786840836363"
	mutationFollowNewsletter       = "9926858900719341"

	// desktop & mobile
	queryFetchNewsletterDesktop        = "9779843322044422"
	queryRecommendedNewslettersDesktop = "27256776790637714"
	querySubscribedNewslettersDesktop  = "8621797084555037"
	queryNewsletterSubscribersDesktop  = "25403502652570342"
	mutationMuteNewsletterDesktop      = "5971669009605755" // variables -> {newsletter_id, updates->{description, settings}}, output: xwa2_newsletter_update -> NewsletterMetadata without viewer meta
	mutationUnmuteNewsletterDesktop    = "6104029483058502"
	mutationUpdateNewsletterDesktop    = "8580212968761751"
	mutationCreateNewsletterDesktop    = "27527996220149684"
	mutationUnfollowNewsletterDesktop  = "6548204731885183"
	mutationFollowNewsletterDesktop    = "6742622672428529"
)

func convertQueryID(cli *Client, queryID string) string {
	if payload := cli.Store.GetClientPayload(); payload.GetUserAgent().GetPlatform() == waWa6.ClientPayload_UserAgent_MACOS || payload.GetWebInfo() == nil {
		switch queryID {
		case queryFetchNewsletter:
			return queryFetchNewsletterDesktop
		case queryRecommendedNewsletters:
			return queryRecommendedNewslettersDesktop
		case querySubscribedNewsletters:
			return querySubscribedNewslettersDesktop
		case queryNewsletterSubscribers:
			return queryNewsletterSubscribersDesktop
		case mutationMuteNewsletter:
			return mutationMuteNewsletterDesktop
		case mutationUnmuteNewsletter:
			return mutationUnmuteNewsletterDesktop
		case mutationUpdateNewsletter:
			return mutationUpdateNewsletterDesktop
		case mutationCreateNewsletter:
			return mutationCreateNewsletterDesktop
		case mutationUnfollowNewsletter:
			return mutationUnfollowNewsletterDesktop
		case mutationFollowNewsletter:
			return mutationFollowNewsletterDesktop
		default:
			return queryID
		}
	} else {
		return queryID
	}
}

func (cli *Client) sendMexIQ(ctx context.Context, queryID string, variables any) (json.RawMessage, error) {
	queryID = convertQueryID(cli, queryID)
	payload, err := json.Marshal(map[string]any{
		"variables": variables,
	})
	if err != nil {
		return nil, err
	}
	resp, err := cli.sendIQ(ctx, infoQuery{
		Namespace: "w:mex",
		Type:      iqGet,
		To:        types.ServerJID,
		Content: []waBinary.Node{{
			Tag: "query",
			Attrs: waBinary.Attrs{
				"query_id": queryID,
			},
			Content: payload,
		}},
	})
	if err != nil {
		return nil, err
	}
	result, ok := resp.GetOptionalChildByTag("result")
	if !ok {
		return nil, &ElementMissingError{Tag: "result", In: "mex response"}
	}
	resultContent, ok := result.Content.([]byte)
	if !ok {
		return nil, fmt.Errorf("unexpected content type %T in mex response", result.Content)
	}
	return decodeMexResponse(queryID, result.AttrGetter().OptionalString("format"), resultContent)
}

func decodeMexResponse(queryID, format string, content []byte) (data json.RawMessage, err error) {
	// Unsupported/corrupt service responses must become action errors, never process exits.
	defer func() {
		if failure := recover(); failure != nil {
			data, err = nil, fmt.Errorf("invalid channel response: %v", failure)
		}
	}()
	if len(content) == 0 || len(content) > 2*1024*1024 {
		return nil, fmt.Errorf("channel response is empty or exceeds the 2 MB limit")
	}
	if format == "argo" {
		typesByName, err := argo.GetStore()
		if err != nil {
			return nil, err
		}
		queryIDMap, err := argo.GetQueryIDToMessageName()
		if err != nil {
			return nil, err
		}
		name := queryIDMap[queryID]
		// Web and desktop queries describe the same channel metadata. A mismatched
		// future schema fails validation rather than producing invented metadata.
		if name == "" {
			switch queryID {
			case queryFetchNewsletter:
				name = "NewsletterMetadata"
			case querySubscribedNewsletters:
				name = "NewsletterSubscribed"
			case mutationMuteNewsletter:
				name = "NewsletterMute"
			case mutationUnmuteNewsletter:
				name = "NewsletterUnmute"
			case mutationFollowNewsletter:
				name = "NewsletterJoin"
			case mutationUnfollowNewsletter:
				name = "NewsletterLeave"
			}
		}
		wt := typesByName[name]
		if wt == nil {
			return nil, fmt.Errorf("unsupported Argo channel schema for query %s", queryID)
		}
		if err := argo.ValidateTypedFrame(content, wt); err != nil {
			return nil, fmt.Errorf("unsupported or invalid Argo channel response: %w", err)
		}
		decoder, err := codec.NewArgoDecoder(buf.NewBufReadonly(content))
		if err != nil {
			return nil, err
		}
		decoded, err := decoder.ArgoToMap(wt)
		if err != nil {
			return nil, fmt.Errorf("could not decode Argo channel response: %w", err)
		}
		content, err = json.Marshal(normalizeMexValue(decoded))
		if err != nil {
			return nil, err
		}
	} else if format != "" && format != "json" {
		return nil, fmt.Errorf("unsupported channel response format %q", format)
	}
	var response types.GraphQLResponse
	if err := json.Unmarshal(content, &response); err != nil {
		return nil, fmt.Errorf("failed to unmarshal graphql response: %w", err)
	}
	if len(response.Errors) > 0 {
		return response.Data, fmt.Errorf("graphql error: %w", response.Errors)
	}
	if len(response.Data) == 0 {
		return nil, fmt.Errorf("channel response has no GraphQL data")
	}
	return response.Data, nil
}

func normalizeMexValue(value any) any {
	switch typed := value.(type) {
	case *orderedmap.OrderedMap[string, any]:
		result := make(map[string]any, typed.Len())
		for element := typed.Front(); element != nil; element = element.Next() {
			result[element.Key] = normalizeMexValue(element.Value)
		}
		return result
	case []any:
		result := make([]any, len(typed))
		for index, element := range typed {
			result[index] = normalizeMexValue(element)
		}
		return result
	default:
		return value
	}
}

func validateNewsletterMutation(data json.RawMessage, channel types.JID, fields ...string) error {
	if channel.IsEmpty() || channel.Server != types.NewsletterServer {
		return fmt.Errorf("invalid channel-operation target")
	}
	var response map[string]json.RawMessage
	if err := json.Unmarshal(data, &response); err != nil {
		return fmt.Errorf("WhatsApp returned an invalid channel-operation confirmation: %w", err)
	}
	for _, field := range fields {
		var confirmation struct {
			ID string `json:"id"`
		}
		if json.Unmarshal(response[field], &confirmation) == nil && confirmation.ID == channel.String() {
			return nil
		}
	}
	return fmt.Errorf("WhatsApp did not confirm this operation for the requested channel")
}

type respGetNewsletterInfo struct {
	Newsletter *types.NewsletterMetadata `json:"xwa2_newsletter"`
}

func (cli *Client) getNewsletterInfo(ctx context.Context, input map[string]any, fetchViewerMeta bool) (*types.NewsletterMetadata, error) {
	data, err := cli.sendMexIQ(ctx, queryFetchNewsletter, map[string]any{
		"fetch_creation_time":   true,
		"fetch_full_image":      true,
		"fetch_viewer_metadata": fetchViewerMeta,
		"input":                 input,
	})
	var respData respGetNewsletterInfo
	if data != nil {
		jsonErr := json.Unmarshal(data, &respData)
		if err == nil && jsonErr != nil {
			err = jsonErr
		}
	}
	if err == nil && (respData.Newsletter == nil || respData.Newsletter.ID.IsEmpty() || respData.Newsletter.ID.Server != types.NewsletterServer) {
		err = fmt.Errorf("WhatsApp returned no channel metadata")
	}
	return respData.Newsletter, err
}

// GetNewsletterInfo gets the info of a newsletter that you're joined to.
func (cli *Client) GetNewsletterInfo(ctx context.Context, jid types.JID) (*types.NewsletterMetadata, error) {
	metadata, err := cli.getNewsletterInfo(ctx, map[string]any{
		"key":  jid.String(),
		"type": types.NewsletterKeyTypeJID,
	}, true)
	if err == nil && metadata.ID != jid {
		return nil, fmt.Errorf("WhatsApp returned metadata for a different channel")
	}
	return metadata, err
}

// GetNewsletterInfoWithInvite gets the info of a newsletter with an invite link.
//
// You can either pass the full link (https://whatsapp.com/channel/...) or just the `...` part.
//
// Note that the ViewerMeta field of the returned NewsletterMetadata will be nil.
func (cli *Client) GetNewsletterInfoWithInvite(ctx context.Context, key string) (*types.NewsletterMetadata, error) {
	return cli.getNewsletterInfo(ctx, map[string]any{
		"key":  stripURLPrefix(key, NewsletterLinkPrefix),
		"type": types.NewsletterKeyTypeInvite,
	}, false)
}

type respGetSubscribedNewsletters struct {
	Newsletters []*types.NewsletterMetadata `json:"xwa2_newsletter_subscribed"`
}

// GetSubscribedNewsletters gets the info of all newsletters that you're joined to.
func (cli *Client) GetSubscribedNewsletters(ctx context.Context) ([]*types.NewsletterMetadata, error) {
	data, err := cli.sendMexIQ(ctx, querySubscribedNewsletters, map[string]any{})
	var respData respGetSubscribedNewsletters
	if data != nil {
		jsonErr := json.Unmarshal(data, &respData)
		if err == nil && jsonErr != nil {
			err = jsonErr
		}
	}
	if err == nil && respData.Newsletters == nil {
		err = fmt.Errorf("WhatsApp returned no followed-channel list")
	}
	return respData.Newsletters, err
}

type CreateNewsletterParams struct {
	Name        string `json:"name"`
	Description string `json:"description,omitempty"`
	Picture     []byte `json:"picture,omitempty"`
}

type respCreateNewsletter struct {
	Newsletter *types.NewsletterMetadata `json:"xwa2_newsletter_create"`
}

// CreateNewsletter creates a new WhatsApp channel.
func (cli *Client) CreateNewsletter(ctx context.Context, params CreateNewsletterParams) (*types.NewsletterMetadata, error) {
	resp, err := cli.sendMexIQ(ctx, mutationCreateNewsletter, map[string]any{
		"newsletter_input": &params,
	})
	if err != nil {
		return nil, err
	}
	var respData respCreateNewsletter
	err = json.Unmarshal(resp, &respData)
	if err != nil {
		return nil, err
	}
	return respData.Newsletter, nil
}

// AcceptTOSNotice accepts a ToS notice.
//
// To accept the terms for creating newsletters, use
//
//	cli.AcceptTOSNotice("20601218", "5")
func (cli *Client) AcceptTOSNotice(ctx context.Context, noticeID, stage string) error {
	_, err := cli.sendIQ(ctx, infoQuery{
		Namespace: "tos",
		Type:      iqSet,
		To:        types.ServerJID,
		Content: []waBinary.Node{{
			Tag: "notice",
			Attrs: waBinary.Attrs{
				"id":    noticeID,
				"stage": stage,
			},
		}},
	})
	return err
}

// NewsletterToggleMute changes the mute status of a newsletter.
func (cli *Client) NewsletterToggleMute(ctx context.Context, jid types.JID, mute bool) error {
	query := mutationUnmuteNewsletter
	if mute {
		query = mutationMuteNewsletter
	}
	data, err := cli.sendMexIQ(ctx, query, map[string]any{
		"newsletter_id": jid.String(),
	})
	if err != nil {
		return err
	}
	field := "xwa2_newsletter_unmute_v2"
	if mute {
		field = "xwa2_newsletter_mute_v2"
	}
	return validateNewsletterMutation(data, jid, field)
}

// FollowNewsletter makes the user follow (join) a WhatsApp channel.
func (cli *Client) FollowNewsletter(ctx context.Context, jid types.JID) error {
	data, err := cli.sendMexIQ(ctx, mutationFollowNewsletter, map[string]any{
		"newsletter_id": jid.String(),
	})
	if err != nil {
		return err
	}
	return validateNewsletterMutation(data, jid, "xwa2_newsletter_join_v2", "xwa2_newsletter_follow")
}

// UnfollowNewsletter makes the user unfollow (leave) a WhatsApp channel.
func (cli *Client) UnfollowNewsletter(ctx context.Context, jid types.JID) error {
	data, err := cli.sendMexIQ(ctx, mutationUnfollowNewsletter, map[string]any{
		"newsletter_id": jid.String(),
	})
	if err != nil {
		return err
	}
	return validateNewsletterMutation(data, jid, "xwa2_newsletter_leave_v2", "xwa2_newsletter_unfollow")
}

type GetNewsletterMessagesParams struct {
	Count  int
	Before types.MessageServerID
}

// GetNewsletterMessages gets messages in a WhatsApp channel.
func (cli *Client) GetNewsletterMessages(ctx context.Context, jid types.JID, params *GetNewsletterMessagesParams) ([]*types.NewsletterMessage, error) {
	attrs := waBinary.Attrs{
		"type": "jid",
		"jid":  jid,
	}
	if params != nil {
		if params.Count != 0 {
			attrs["count"] = params.Count
		}
		if params.Before != 0 {
			attrs["before"] = params.Before
		}
	}
	resp, err := cli.sendIQ(ctx, infoQuery{
		Namespace: "newsletter",
		Type:      iqGet,
		To:        types.ServerJID,
		Content: []waBinary.Node{{
			Tag:   "messages",
			Attrs: attrs,
		}},
	})
	if err != nil {
		return nil, err
	}
	messages, ok := resp.GetOptionalChildByTag("messages")
	if !ok {
		return nil, &ElementMissingError{Tag: "messages", In: "newsletter messages response"}
	}
	return cli.parseNewsletterMessages(&messages), nil
}

type GetNewsletterUpdatesParams struct {
	Count int
	Since time.Time
	After types.MessageServerID
}

// GetNewsletterMessageUpdates gets updates in a WhatsApp channel.
//
// These are the same kind of updates that NewsletterSubscribeLiveUpdates triggers (reaction and view counts).
func (cli *Client) GetNewsletterMessageUpdates(ctx context.Context, jid types.JID, params *GetNewsletterUpdatesParams) ([]*types.NewsletterMessage, error) {
	attrs := waBinary.Attrs{}
	if params != nil {
		if params.Count != 0 {
			attrs["count"] = params.Count
		}
		if !params.Since.IsZero() {
			attrs["since"] = params.Since.Unix()
		}
		if params.After != 0 {
			attrs["after"] = params.After
		}
	}
	resp, err := cli.sendIQ(ctx, infoQuery{
		Namespace: "newsletter",
		Type:      iqGet,
		To:        jid,
		Content: []waBinary.Node{{
			Tag:   "message_updates",
			Attrs: attrs,
		}},
	})
	if err != nil {
		return nil, err
	}
	messages, ok := resp.GetOptionalChildByTag("message_updates", "messages")
	if !ok {
		return nil, &ElementMissingError{Tag: "messages", In: "newsletter messages response"}
	}
	return cli.parseNewsletterMessages(&messages), nil
}
