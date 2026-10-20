package main

import (
	"context"
	"encoding/json"
	"fmt"

	"strconv"
	"strings"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/appstate"
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
			r.Items = append(r.Items, action("Mute chat", "Mute indefinitely on your linked devices", "mute_chat", map[string]string{"muted": "true"}, nil, true))
			r.Items = append(r.Items, action("Unmute chat", "Restore notifications", "mute_chat", map[string]string{"muted": "false"}, nil, true))
			r.Items = append(r.Items, action("Mark unread on WhatsApp", "Set WhatsApp's chat marker; nchat keeps read messages read", "mark_unread", nil, nil, true))
			if strings.HasSuffix(p["chatId"], "@g.us") {
				r.Items = append(r.Items, action("Group information", "Members, settings and invitations", "group_info", nil, nil, false))
			}
		}
		if p["msgId"] != "" {
			r.Items = append(r.Items, action("Star message", "Save in WhatsApp starred messages", "star_message", map[string]string{"starred": "true"}, nil, true))
			r.Items = append(r.Items, action("Unstar message", "Remove from starred messages", "star_message", map[string]string{"starred": "false"}, nil, true))
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
	case "mute_chat":
		muted := actionBool(p["muted"])
		if err = cli.SendAppState(ctx, appstate.BuildMute(chat, muted, 0)); err != nil {
			return actionError("Chat notifications", err)
		}
		CWmUpdateMuteNotify(connID, p["chatId"], BoolToInt(muted))
		return actionOK("Chat notifications", "Notification setting updated.")
	case "mark_unread":
		if err = cli.SendAppState(ctx, appstate.BuildMarkChatAsRead(chat, false, time.Time{}, nil)); err != nil {
			return actionError("Mark unread", err)
		}
		return actionOK("Marked unread on WhatsApp", "Marked unread on WhatsApp; this does not make previously read messages unread in nchat.")
	case "star_message":
		if p["msgId"] == "" {
			return actionError("Star message", fmt.Errorf("select a message first"))
		}
		sender, _ := types.ParseJID(p["senderId"])
		if err = cli.SendAppState(ctx, appstate.BuildStar(chat, sender, p["msgId"], actionBool(p["isOutgoing"]) || actionBool(p["fromMe"]), actionBool(p["starred"]))); err != nil {
			return actionError("Star message", err)
		}
		return actionOK("Starred message", "WhatsApp starred-message setting updated.")
	case "group_info", "group_invite", "group_name", "group_description", "group_announce", "group_locked", "group_member", "group_leave":
		return wmGroupAction(ctx, connID, cli, op, p, chat)
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

func validPhoneNumber(phone string) bool {
	digits := 0
	for i, r := range phone {
		if r >= '0' && r <= '9' {
			digits++
		} else if r != ' ' && r != '-' && r != '(' && r != ')' && !(r == '+' && i == 0) {
			return false
		}
	}
	return digits >= 7 && digits <= 15
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

func wmGroupAction(ctx context.Context, connID int, cli *whatsmeow.Client, op string, p map[string]string, chat types.JID) actionResult {
	if chat.Server != types.GroupServer {
		return actionError("Group", fmt.Errorf("choose a WhatsApp group"))
	}
	info, err := cli.GetGroupInfo(ctx, chat)
	if err != nil {
		return actionError("Group", err)
	}
	admin := false
	for _, member := range info.Participants {
		if (member.IsAdmin || member.IsSuperAdmin) && (IsSelfUser(cli, member.JID.String()) || IsSelfUser(cli, member.PhoneNumber.String()) || IsSelfUser(cli, member.LID.String())) {
			admin = true
		}
	}
	if op == "group_info" {
		r := actionOK(info.Name, info.Topic)
		for _, m := range info.Participants {
			id := m.JID.String()
			name := actionContactName(connID, id)
			if name == id && m.DisplayName != "" {
				name = SanitizeName(m.DisplayName)
			}
			role := "Member"
			if m.IsAdmin || m.IsSuperAdmin {
				role = "Admin"
			}
			r.Items = append(r.Items, action(name, role, "open_chat", map[string]string{"chatId": id}, nil, false))
		}
		r.Items = append(r.Items, action("Leave group", "Leave this group on WhatsApp", "group_leave", nil, nil, true))
		if admin {
			r.Items = append(r.Items,
				action("Invite link", "Retrieve the current group invite link", "group_invite", nil, nil, false),
				action("Change group name", "Set a new group name", "group_name", nil, []actionField{{"name", "Group name", info.Name}}, true),
				action("Change description", "Set the group description", "group_description", nil, []actionField{{"description", "Description", info.Topic}}, true),
				action("Set who can send", "true = admins only; false = all members", "group_announce", nil, []actionField{{"enabled", "Admins only (true/false)", strconv.FormatBool(info.IsAnnounce)}}, true),
				action("Set who can edit group info", "true = admins only; false = all members", "group_locked", nil, []actionField{{"enabled", "Admins only (true/false)", strconv.FormatBool(info.IsLocked)}}, true),
				action("Manage participant", "Add, remove, promote or demote", "group_member", nil, []actionField{{"member", "Member phone number or WhatsApp ID", ""}, {"change", "add / remove / promote / demote", "add"}}, true))
		}
		return r
	}
	if op == "group_leave" {
		err = cli.LeaveGroup(ctx, chat)
	} else {
		if !admin {
			return actionError("Group", fmt.Errorf("this action requires group administrator permission"))
		}
		switch op {
		case "group_invite":
			link, err := cli.GetGroupInviteLink(ctx, chat, false)
			if err != nil {
				return actionError("Group invite", err)
			}
			return actionOK("Group invite", link)
		case "group_name":
			name := strings.TrimSpace(p["name"])
			if name == "" || len([]rune(name)) > 100 {
				return actionError("Group name", fmt.Errorf("enter a name of 1–100 characters"))
			}
			err = cli.SetGroupName(ctx, chat, name)
		case "group_description":
			err = cli.SetGroupDescription(ctx, chat, p["description"])
		case "group_announce", "group_locked":
			if p["enabled"] != "true" && p["enabled"] != "false" {
				return actionError("Group setting", fmt.Errorf("enter true or false"))
			}
			if op == "group_announce" {
				err = cli.SetGroupAnnounce(ctx, chat, actionBool(p["enabled"]))
			} else {
				err = cli.SetGroupLocked(ctx, chat, actionBool(p["enabled"]))
			}
		case "group_member":
			id := strings.TrimSpace(p["member"])
			if !strings.Contains(id, "@") {
				if !validPhoneNumber(id) {
					return actionError("Participant", fmt.Errorf("enter a full phone number or WhatsApp ID"))
				}
				id = strings.Map(func(r rune) rune {
					if r >= '0' && r <= '9' {
						return r
					}
					return -1
				}, id) + "@s.whatsapp.net"
			}
			member, parseErr := types.ParseJID(id)
			if parseErr != nil || member.IsEmpty() || (member.Server != types.DefaultUserServer && member.Server != types.HiddenUserServer) {
				return actionError("Participant", fmt.Errorf("invalid participant ID"))
			}
			change := whatsmeow.ParticipantChange(p["change"])
			if change != whatsmeow.ParticipantChangeAdd && change != whatsmeow.ParticipantChangeRemove && change != whatsmeow.ParticipantChangePromote && change != whatsmeow.ParticipantChangeDemote {
				return actionError("Participant", fmt.Errorf("choose add, remove, promote or demote"))
			}
			var changes []types.GroupParticipant
			changes, err = cli.UpdateGroupParticipants(ctx, chat, []types.JID{member}, change)
			if err == nil {
				for _, result := range changes {
					if result.Error != 0 {
						return actionError("Participant", fmt.Errorf("WhatsApp rejected the change (code %d)", result.Error))
					}
				}
			}
		}
	}
	if err != nil {
		return actionError("Group", err)
	}
	if op == "group_leave" {
		CWmUpdateMuteNotify(connID, chat.String(), 1)
	}
	return actionOK("Group", "WhatsApp confirmed the change.")
}
