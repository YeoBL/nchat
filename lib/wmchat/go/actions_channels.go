package main

import (
	"context"

	"fmt"

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
	}
	return actionResult{}, false
}
