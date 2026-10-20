package main

import (
	"context"
	"encoding/json"
	"fmt"

	"strconv"
	"strings"
	"time"

	"go.mau.fi/whatsmeow"

	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
)

type actionField struct {
	Key   string `json:"key"`
	Label string `json:"label"`
	Value string `json:"value"`
}

type actionItem struct {
	Title     string            `json:"title"`
	Detail    string            `json:"detail"`
	Operation string            `json:"operation"`
	Params    map[string]string `json:"params"`
	Fields    []actionField     `json:"fields"`
	Confirm   bool              `json:"confirm"`
}

type actionResult struct {
	Success  bool         `json:"success"`
	Title    string       `json:"title"`
	Text     string       `json:"text"`
	FilePath string       `json:"filePath"`
	Items    []actionItem `json:"items"`
}

func action(title, detail, operation string, params map[string]string, fields []actionField, confirm bool) actionItem {
	return actionItem{Title: title, Detail: detail, Operation: operation, Params: params, Fields: fields, Confirm: confirm}
}

func actionOK(title, text string) actionResult {
	return actionResult{Success: true, Title: title, Text: text}
}

func actionError(title string, err error) actionResult {
	return actionResult{Title: title, Text: err.Error()}
}

func WmClientAction(connID int, operation, rawParams string) string {
	p := make(map[string]string)
	if err := json.Unmarshal([]byte(rawParams), &p); err != nil {
		data, _ := json.Marshal(actionError("WhatsApp", fmt.Errorf("invalid action parameters")))
		return string(data)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 55*time.Second)
	defer cancel()
	result := wmClientAction(ctx, connID, operation, p)
	data, err := json.Marshal(result)
	if err != nil {
		return `{"success":false,"title":"WhatsApp","text":"Could not read account tools."}`
	}
	return string(data)
}

func actionChat(p map[string]string) (types.JID, error) {
	id, err := types.ParseJID(p["chatId"])
	if err != nil || id.IsEmpty() {
		return types.EmptyJID, fmt.Errorf("choose a chat first")
	}
	return id, nil
}

func actionBool(value string) bool { return value == "1" || value == "true" || value == "yes" }

func actionContactName(connID int, id string) string {
	if name := GetContactName(connID, id); name != "" && name != id {
		return name
	}
	jid, err := types.ParseJID(id)
	if err == nil && jid.Server == types.DefaultUserServer {
		return "+" + jid.User
	}
	return id
}

func actionContactName(connID int, id string) string {
	if name := GetContactName(connID, id); name != "" && name != id {
		return name
	}
	jid, err := types.ParseJID(id)
	if err == nil && jid.Server == types.DefaultUserServer {
		return "+" + jid.User
	}
	return id
}

func wmClientAction(ctx context.Context, connID int, op string, p map[string]string) actionResult {
	if op == "menu" {
		r := actionOK("WhatsApp tools", "Choose an account or chat action.")
		r.Items = append(r.Items, action("Status updates", "Browse updates received by this device", "status_list", nil, nil, false))
		r.Items = append(r.Items, action("Channels", "Browse followed channels and fetch their posts", "channel_list", nil, nil, false))
		r.Items = append(r.Items, action("Communities", "Browse your communities and their groups", "community_list", nil, nil, false))
		if p["chatId"] != "" {
			r.Items = append(r.Items, action("Create poll", "Send a new poll to this chat", "create_poll", nil, []actionField{{"question", "Question", ""}, {"options", "Choices separated by |", ""}, {"maxChoices", "Maximum choices (0 = any)", "1"}}, true))
		}
		if p["msgId"] != "" {
		}
		return r
	}
	cli := GetClient(connID)
	if cli == nil {
		return actionError("WhatsApp", fmt.Errorf("account is not connected"))
	}
	if r, handled := wmChannelAction(ctx, connID, cli, op, p); handled {
		return r
	}
	if r, handled := wmStatusAction(ctx, connID, cli, op, p); handled {
		return r
	}
	if r, handled := wmMessageAction(ctx, connID, cli, op, p); handled {
		return r
	}
	chat, err := actionChat(p)
	if err != nil {
		return actionError("WhatsApp", err)
	}
	if chat.Server == types.NewsletterServer && op != "history_request" {
		return actionError("WhatsApp channel", fmt.Errorf("use Channel tools for this channel"))
	}
	_ = chat
	switch op {
	case "create_poll":
		options, limit, err := validateNewPoll(p["question"], p["options"], p["maxChoices"])
		if err != nil {
			return actionError("Create poll", err)
		}
		return sendActionMessage(ctx, connID, cli, chat, cli.BuildPollCreation(strings.TrimSpace(p["question"]), options, limit), "Poll sent")
	}
	return actionError("WhatsApp", fmt.Errorf("unknown action"))
}

func validateNewPoll(question, rawOptions, rawLimit string) ([]string, int, error) {
	if strings.TrimSpace(question) == "" || len([]rune(question)) > 255 {
		return nil, 0, fmt.Errorf("question must contain 1–255 characters")
	}
	parts := strings.Split(rawOptions, "|")
	options := make([]string, 0, len(parts))
	seen := make(map[string]bool)
	for _, part := range parts {
		option := strings.TrimSpace(part)
		if option == "" || seen[option] || len([]rune(option)) > 100 {
			return nil, 0, fmt.Errorf("choices must be unique, nonempty and at most 100 characters")
		}
		seen[option] = true
		options = append(options, option)
	}
	if len(options) < 2 || len(options) > 12 {
		return nil, 0, fmt.Errorf("enter between 2 and 12 choices")
	}
	limit, err := strconv.Atoi(rawLimit)
	if err != nil || limit < 0 || limit > len(options) {
		return nil, 0, fmt.Errorf("maximum choices must be 0–%d", len(options))
	}
	return options, limit, nil
}

func sendActionMessage(ctx context.Context, connID int, cli *whatsmeow.Client, chat types.JID, msg *waE2E.Message, title string) actionResult {
	response, err := cli.SendMessage(ctx, chat, msg)
	if err != nil {
		return actionError(title, err)
	}
	if cli.Store.ID != nil {
		if handler := GetHandler(connID); handler != nil {
			handler.HandleMessage(types.MessageInfo{MessageSource: types.MessageSource{Chat: chat, Sender: *cli.Store.ID, IsFromMe: true, IsGroup: chat.Server == types.GroupServer}, ID: response.ID, Timestamp: response.Timestamp}, msg, true)
		}
	}
	return actionOK(title, "Sent to WhatsApp.")
}
