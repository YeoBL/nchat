package main

import (
	"context"
	"crypto/sha256"
	"encoding/gob"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"google.golang.org/protobuf/proto"
)

// Statuses are deliberately separate from chat history: they expire after 24 hours.
type storedStatus struct {
	ID, Sender string
	Timestamp  int64
	FromMe     bool
	Message    []byte
}

var statusStore = struct {
	sync.Mutex
	profiles map[string]map[string]storedStatus
}{profiles: make(map[string]map[string]storedStatus)}

func statusKey(sender, id string) string {
	hash := sha256.Sum256([]byte(sender + "\x00" + id))
	return hex.EncodeToString(hash[:])
}

// Caller holds statusStore. The on-disk store also contains only unexpired statuses.
func loadStatuses(path string) map[string]storedStatus {
	if values, ok := statusStore.profiles[path]; ok {
		return values
	}
	values := make(map[string]storedStatus)
	if file, err := os.Open(filepath.Join(path, "status-updates.dat")); err == nil {
		if gob.NewDecoder(file).Decode(&values) != nil || values == nil {
			values = make(map[string]storedStatus)
		}
		file.Close()
	}
	statusStore.profiles[path] = values
	return values
}

func pruneStatuses(values map[string]storedStatus, now time.Time) bool {
	changed := false
	for key, value := range values {
		if value.Timestamp <= now.Add(-24*time.Hour).Unix() {
			delete(values, key)
			changed = true
		}
	}
	return changed
}

func saveStatuses(path string, values map[string]storedStatus) {
	file, err := os.CreateTemp(path, ".status-*.tmp")
	if err != nil {
		LOG_WARNING(fmt.Sprintf("save statuses: %v", err))
		return
	}
	defer os.Remove(file.Name())
	if err = gob.NewEncoder(file).Encode(values); err == nil {
		err = file.Close()
	} else {
		file.Close()
	}
	if err == nil {
		err = os.Rename(file.Name(), filepath.Join(path, "status-updates.dat"))
	}
	if err != nil {
		LOG_WARNING(fmt.Sprintf("save statuses: %v", err))
	}
}

func recordStatusMessage(connId int, info types.MessageInfo, msg *waE2E.Message) {
	if info.Chat != types.StatusBroadcastJID || msg == nil {
		return
	}
	path := GetPath(connId)
	if path == "" {
		return
	}
	statusStore.Lock()
	defer statusStore.Unlock()
	values := loadStatuses(path)
	changed := pruneStatuses(values, time.Now())
	if revoke := msg.GetProtocolMessage(); revoke != nil && revoke.GetType() == waE2E.ProtocolMessage_REVOKE {
		for key, value := range values {
			if value.ID == revoke.GetKey().GetID() {
				delete(values, key)
				changed = true
			}
		}
	} else if info.ID != "" && !info.Timestamp.IsZero() && info.Timestamp.After(time.Now().Add(-24*time.Hour)) &&
		(msg.Conversation != nil || msg.ExtendedTextMessage != nil || msg.ImageMessage != nil ||
			msg.VideoMessage != nil || msg.AudioMessage != nil) {
		encoded, err := proto.Marshal(msg)
		if err == nil {
			sender := info.Sender.ToNonAD().String()
			values[statusKey(sender, info.ID)] = storedStatus{info.ID, sender, info.Timestamp.Unix(), info.IsFromMe, encoded}
			changed = true
		}
	}
	if changed {
		saveStatuses(path, values)
	}
}

func statusSummary(msg *waE2E.Message) string {
	switch {
	case msg.ImageMessage != nil:
		return strings.TrimSpace("Photo " + msg.ImageMessage.GetCaption())
	case msg.VideoMessage != nil:
		return strings.TrimSpace("Video " + msg.VideoMessage.GetCaption())
	case msg.AudioMessage != nil:
		return "Voice status"
	case msg.ExtendedTextMessage != nil:
		return msg.ExtendedTextMessage.GetText()
	default:
		return msg.GetConversation()
	}
}

func statusPrivacyText(connId int, setting types.StatusPrivacy) string {
	var label string
	switch setting.Type {
	case types.StatusPrivacyTypeContacts:
		label = "My contacts"
	case types.StatusPrivacyTypeBlacklist:
		label = "My contacts except"
	case types.StatusPrivacyTypeWhitelist:
		label = "Only share with"
	default:
		label = string(setting.Type)
	}
	if len(setting.List) > 0 {
		names := make([]string, 0, len(setting.List))
		for _, jid := range setting.List {
			names = append(names, actionContactName(connId, jid.ToNonAD().String()))
		}
		sort.Strings(names)
		label += ": " + strings.Join(names, ", ")
	} else if setting.Type == types.StatusPrivacyTypeWhitelist {
		label += ": nobody"
	} else if setting.Type == types.StatusPrivacyTypeBlacklist {
		label += ": nobody excluded"
	}
	return label
}

func statusAudienceToken(setting types.StatusPrivacy) string {
	users := make([]string, 0, len(setting.List))
	for _, jid := range setting.List {
		users = append(users, jid.String())
	}
	sort.Strings(users)
	return statusKey(string(setting.Type), strings.Join(users, "\x00"))
}

func wmStatusAction(ctx context.Context, connId int, cli *whatsmeow.Client, op string, p map[string]string) (actionResult, bool) {
	if !strings.HasPrefix(op, "status_") {
		return actionResult{}, false
	}
	if cli == nil {
		return actionError("Status", errors.New("WhatsApp is not connected")), true
	}
	switch op {
	case "status_privacy":
		settings, err := cli.GetStatusPrivacy(ctx)
		if err != nil {
			return actionError("Status audience", err), true
		}
		var lines []string
		for i, setting := range settings {
			label := statusPrivacyText(connId, setting)
			if i == 0 {
				label = "Current configured status audience: " + label
			}
			lines = append(lines, label)
		}
		return actionOK("Status audience", strings.Join(lines, "\n\n")), true
	case "status_post_text", "status_post_media":
		return postStatus(ctx, connId, cli, op, p), true
	case "status_list", "status_item", "status_media":
		path := GetPath(connId)
		if path == "" {
			return actionError("Status", errors.New("WhatsApp profile is unavailable")), true
		}
		statusStore.Lock()
		values := loadStatuses(path)
		if pruneStatuses(values, time.Now()) {
			saveStatuses(path, values)
		}
		entries := make(map[string]storedStatus, len(values))
		for key, value := range values {
			entries[key] = value
		}
		statusStore.Unlock()
		if op == "status_list" {
			result := actionOK("WhatsApp Status", "Statuses received by this nchat account in the last 24 hours.")
			keys := make([]string, 0, len(entries))
			for key := range entries {
				keys = append(keys, key)
			}
			sort.Slice(keys, func(i, j int) bool { return entries[keys[i]].Timestamp > entries[keys[j]].Timestamp })
			for _, key := range keys {
				entry := entries[key]
				var msg waE2E.Message
				if proto.Unmarshal(entry.Message, &msg) != nil {
					continue
				}
				name := actionContactName(connId, entry.Sender)
				if entry.FromMe {
					name = "My status"
				}
				result.Items = append(result.Items, action(name+" · "+time.Unix(entry.Timestamp, 0).Format("15:04"),
					statusSummary(&msg), "status_item", map[string]string{"status_id": key}, nil, false))
			}
			result.Items = append(result.Items,
				action("Post text status", "Choose text and review the current configured status audience", "status_post_text", nil, nil, false),
				action("Post photo/video status", "Choose a file and review the current configured status audience", "status_post_media", nil, nil, false),
				action("Status audience", "View the configured audience and lists", "status_privacy", nil, nil, false))
			return result, true
		}
		entry, ok := entries[p["status_id"]]
		if !ok {
			return actionError("Status", errors.New("This status expired or is no longer available")), true
		}
		var msg waE2E.Message
		if err := proto.Unmarshal(entry.Message, &msg); err != nil {
			return actionError("Status", err), true
		}
		result := actionOK(actionContactName(connId, entry.Sender), statusSummary(&msg))
		var media whatsmeow.DownloadableMessage
		extension := ""
		switch {
		case msg.ImageMessage != nil && !msg.ImageMessage.GetViewOnce():
			media, extension = msg.ImageMessage, ExtensionByType(msg.ImageMessage.GetMimetype(), ".jpg")
		case msg.VideoMessage != nil && !msg.VideoMessage.GetViewOnce():
			media, extension = msg.VideoMessage, ExtensionByType(msg.VideoMessage.GetMimetype(), ".mp4")
		case msg.AudioMessage != nil && !msg.AudioMessage.GetViewOnce():
			media, extension = msg.AudioMessage, ExtensionByType(msg.AudioMessage.GetMimetype(), ".ogg")
		}
		if media != nil {
			if op == "status_media" {
				mediaPath := filepath.Join(path, "tmp", "status-"+p["status_id"]+extension)
				if err := os.MkdirAll(filepath.Dir(mediaPath), 0700); err != nil {
					return actionError("Status media", err), true
				}
				fileID := DownloadableMessageToFileId(cli, media, mediaPath)
				filePath, state := DownloadFromFileId(connId, fileID)
				if state != FileStatusDownloaded || filePath == "" {
					return actionError("Status media", errors.New("Could not download this status; its media may have expired")), true
				}
				result.FilePath = filePath
			} else {
				result.Items = append(result.Items, action("Open media", "Open this status in the configured viewer", "status_media", p, nil, false))
			}
		}
		return result, true
	default:
		return actionError("Status", errors.New("Unknown status action")), true
	}
}

func postStatus(ctx context.Context, connId int, cli *whatsmeow.Client, op string, p map[string]string) actionResult {
	settings, err := cli.GetStatusPrivacy(ctx)
	if err != nil {
		return actionError("Post status", err)
	}
	if len(settings) == 0 {
		return actionError("Post status", errors.New("WhatsApp did not provide a status audience"))
	}
	if settings[0].Type != types.StatusPrivacyTypeContacts && settings[0].Type != types.StatusPrivacyTypeBlacklist &&
		settings[0].Type != types.StatusPrivacyTypeWhitelist {
		return actionError("Post status", errors.New("This status audience type is unsupported; choose an audience in WhatsApp first"))
	}
	audience := statusPrivacyText(connId, settings[0])
	token := statusAudienceToken(settings[0])
	if p["audience_token"] != token {
		fields := []actionField{{Key: "text", Label: "Text", Value: p["text"]}}
		if op == "status_post_media" {
			fields = []actionField{{Key: "file_path", Label: "Photo/video file path", Value: p["file_path"]},
				{Key: "text", Label: "Caption (optional)", Value: p["text"]}}
		}
		result := actionOK("Post WhatsApp status", "Current configured status audience: "+audience)
		result.Items = []actionItem{action("Publish status", "Current configured status audience: "+audience,
			op, map[string]string{"audience_token": token}, fields, true)}
		return result
	}
	if settings[0].Type == types.StatusPrivacyTypeWhitelist && len(settings[0].List) == 0 {
		return actionError("Post status", errors.New("The configured audience contains nobody; change it in WhatsApp first"))
	}
	text := strings.TrimSpace(p["text"])
	if len([]rune(text)) > 700 {
		return actionError("Post status", errors.New("Status text must be 700 characters or fewer"))
	}
	message := &waE2E.Message{}
	if op == "status_post_text" {
		if text == "" {
			return actionError("Post status", errors.New("Enter status text first"))
		}
		message.ExtendedTextMessage = &waE2E.ExtendedTextMessage{Text: proto.String(text),
			BackgroundArgb: proto.Uint32(0xff143d59), Font: waE2E.ExtendedTextMessage_SYSTEM.Enum()}
	} else {
		file, err := os.Open(strings.TrimSpace(p["file_path"]))
		if err != nil {
			return actionError("Post status", err)
		}
		defer file.Close()
		info, err := file.Stat()
		if err != nil || !info.Mode().IsRegular() {
			return actionError("Post status", errors.New("Choose a regular photo or video file"))
		}
		const limit = 50 * 1024 * 1024
		if info.Size() > limit {
			return actionError("Post status", errors.New("Choose a file no larger than 50 MB"))
		}
		data, err := io.ReadAll(io.LimitReader(file, limit+1))
		if err != nil || len(data) > limit {
			return actionError("Post status", errors.New("Could not read this file within the 50 MB limit"))
		}
		mimeType := http.DetectContentType(data)
		mediaType := whatsmeow.MediaImage
		if strings.HasPrefix(mimeType, "video/") {
			mediaType = whatsmeow.MediaVideo
		} else if !strings.HasPrefix(mimeType, "image/") {
			return actionError("Post status", errors.New("Choose a supported photo or video file (for example JPEG, PNG or MP4)"))
		}
		uploaded, err := cli.Upload(ctx, data, mediaType)
		if err != nil {
			return actionError("Post status", err)
		}
		if mediaType == whatsmeow.MediaImage {
			message.ImageMessage = &waE2E.ImageMessage{Caption: proto.String(text), Mimetype: proto.String(mimeType),
				URL: proto.String(uploaded.URL), DirectPath: proto.String(uploaded.DirectPath), MediaKey: uploaded.MediaKey,
				FileEncSHA256: uploaded.FileEncSHA256, FileSHA256: uploaded.FileSHA256, FileLength: proto.Uint64(uint64(len(data)))}
		} else {
			message.VideoMessage = &waE2E.VideoMessage{Caption: proto.String(text), Mimetype: proto.String(mimeType),
				URL: proto.String(uploaded.URL), DirectPath: proto.String(uploaded.DirectPath), MediaKey: uploaded.MediaKey,
				FileEncSHA256: uploaded.FileEncSHA256, FileSHA256: uploaded.FileSHA256, FileLength: proto.Uint64(uint64(len(data)))}
		}
	}
	response, err := cli.SendMessage(ctx, types.StatusBroadcastJID, message)
	if err != nil {
		return actionError("Post status", err)
	}
	info := types.MessageInfo{ID: response.ID, Timestamp: response.Timestamp,
		MessageSource: types.MessageSource{Chat: types.StatusBroadcastJID, IsFromMe: true}}
	if cli.Store.ID != nil {
		info.Sender = cli.Store.ID.ToNonAD()
	}
	if info.Timestamp.IsZero() {
		info.Timestamp = time.Now()
	}
	if handler := GetHandler(connId); handler != nil {
		handler.HandleMessage(info, message, true)
	} else {
		recordStatusMessage(connId, info, message)
	}
	return actionOK("Status published", "Shared with: "+audience)
}
