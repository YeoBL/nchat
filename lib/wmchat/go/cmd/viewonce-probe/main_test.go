package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	signalLog "go.mau.fi/libsignal/logger"
	"go.mau.fi/whatsmeow"
	waBinary "go.mau.fi/whatsmeow/binary"
	"go.mau.fi/whatsmeow/proto/waCompanionReg"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/proto/waWa6"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
)

func TestIsolatedCapabilities(t *testing.T) {
	base, props, logger := store.BaseClientPayload, store.DeviceProps, signalLog.Logger
	t.Cleanup(func() { store.BaseClientPayload, store.DeviceProps, signalLog.Logger = base, props, logger })
	client := configureClient(&store.Device{}, false)
	if store.BaseClientPayload == base || store.DeviceProps == props ||
		store.BaseClientPayload.GetUserAgent().GetPlatform() != waWa6.ClientPayload_UserAgent_ANDROID || store.BaseClientPayload.WebInfo != nil ||
		store.DeviceProps.GetPlatformType() != waCompanionReg.DeviceProps_ANDROID_PHONE ||
		store.DeviceProps.GetOs() != "nchat isolated delivery test" || store.DeviceProps.GetRequireFullSync() ||
		proto.Size(store.DeviceProps.GetHistorySyncConfig()) != 0 || client.QRClientType != whatsmeow.PairClientChrome {
		t.Fatal("experimental registration or independent Chrome QR configuration changed")
	}
	if !client.ManualHistorySyncDownload || !client.DisableManualHistorySyncReceipt ||
		client.AutomaticMessageRerequestFromPhone || client.EnableDecryptedEventBuffer || client.UseRetryMessageStore {
		t.Fatal("probe enabled automatic history download or payload buffering")
	}
	if _, ok := signalLog.Logger.(discardSignalLog); !ok {
		t.Fatal("libsignal can print private protocol logs")
	}
	control := configureClient(&store.Device{}, true)
	if store.BaseClientPayload.GetUserAgent().GetPlatform() != waWa6.ClientPayload_UserAgent_WEB ||
		store.BaseClientPayload.GetWebInfo().GetWebSubPlatform() != waWa6.ClientPayload_WebInfo_WEB_BROWSER ||
		store.DeviceProps.GetPlatformType() != waCompanionReg.DeviceProps_CHROME ||
		store.DeviceProps.GetOs() != "nchat isolated login control" ||
		control.QRClientType != whatsmeow.PairClientChrome ||
		!control.ManualHistorySyncDownload || !control.DisableManualHistorySyncReceipt ||
		control.AutomaticMessageRerequestFromPhone || control.EnableDecryptedEventBuffer || control.UseRetryMessageStore {
		t.Fatal("normal login control changed registration or privacy limits")
	}
}

func TestConnectionDiagnosticsNeverPrintPrivateFields(t *testing.T) {
	private := "private-ID-key-phone-error"
	raw := &waBinary.Node{Tag: private}
	for _, fixture := range []struct {
		event any
		want  string
	}{
		{&events.PairSuccess{BusinessName: private}, "pair-success"},
		{&events.PairError{BusinessName: private, Error: errors.New(private)}, "pair-error"},
		{&events.ConnectFailure{Reason: 409, Message: private, Raw: raw}, "connect-failure 409"},
		{&events.LoggedOut{Reason: 401}, "logged-out 401"},
		{&events.TemporaryBan{Code: 101}, "temporary-ban 101"},
		{&events.ClientOutdated{}, "client-outdated"},
		{&events.QRScannedWithoutMultidevice{}, "scanned-without-multidevice"},
		{&events.StreamError{Code: "515", Raw: raw}, "stream-error 515"},
		{&events.StreamError{Code: private, Raw: raw}, "stream-error"},
		{&events.StreamError{Code: "1234567890", Raw: raw}, "stream-error"},
		{&events.Message{Message: &waE2E.Message{Conversation: &private}}, ""},
		{nil, ""},
	} {
		if got := connectionDiagnostic(fixture.event); got != fixture.want {
			t.Fatalf("unexpected diagnostic: %q", got)
		}
	}
}

func TestSenderFilteringFreshMetadataAndRedaction(t *testing.T) {
	allowed, err := allowedSenders([]string{"100@s.whatsapp.net", "200@lid"})
	if err != nil {
		t.Fatal(err)
	}
	start := time.Unix(1000, 900000000)
	info := types.MessageInfo{MessageSource: types.MessageSource{Chat: types.NewJID("100", types.DefaultUserServer), Sender: types.NewJID("other", types.HiddenUserServer), SenderAlt: types.NewADJID("100", 0, 9)}, Timestamp: time.Unix(1000, 0), ID: "private-message-ID", PushName: "private-name"}
	image := &waE2E.ImageMessage{Caption: proto.String("private-caption"), URL: proto.String("https://example.invalid/private-path"), DirectPath: proto.String("private-path"), MediaKey: bytes.Repeat([]byte{1}, 32), FileSHA256: bytes.Repeat([]byte{2}, 32), FileEncSHA256: bytes.Repeat([]byte{3}, 32)}
	message := &events.Message{Info: info, Message: &waE2E.Message{ViewOnceMessageV2: &waE2E.FutureProofMessage{Message: &waE2E.Message{ImageMessage: image}}}}
	record, ok := eventMetadata(message, allowed, start)
	if !ok || !record.ViewOnce || !record.HasImage || !record.HasURL || !record.HasPath || !record.HasKey || !record.HasPlainHash || !record.HasEncodedHash || record.Unavailable {
		t.Fatalf("fresh alias-addressed delivery flags missing: %+v", record)
	}
	encoded, err := json.Marshal(record)
	if err != nil || bytes.Contains(encoded, []byte("private")) || bytes.Contains(encoded, []byte("100")) || bytes.Contains(encoded, []byte("200")) {
		t.Fatal("metadata leaked an identity, content or key")
	}
	var fields map[string]any
	if json.Unmarshal(encoded, &fields) != nil || len(fields) != 9 {
		t.Fatal("unexpected nonmetadata output fields")
	}
	image.MediaKey, image.FileSHA256, image.FileEncSHA256 = []byte{1}, []byte{2}, []byte{3}
	if short, ok := eventMetadata(message, allowed, start); !ok || short.HasKey || short.HasPlainHash || short.HasEncodedHash {
		t.Fatal("short key/hash values reported as usable shape")
	}
	for _, server := range []string{types.GroupServer, types.BroadcastServer, types.NewsletterServer} {
		message.Info.Chat = types.NewJID("100", server)
		if _, ok := eventMetadata(message, allowed, start); ok {
			t.Fatal("non-DM delivery passed filter")
		}
	}
	message.Info.Chat = types.NewJID("100", types.DefaultUserServer)
	message.Info.IsGroup = true
	if _, ok := eventMetadata(message, allowed, start); ok {
		t.Fatal("group source passed direct-message filter")
	}
	message.Info.IsGroup = false
	message.Info.IsFromMe = true
	if _, ok := eventMetadata(message, allowed, start); ok {
		t.Fatal("outgoing message passed sender filter")
	}
	message.Info.IsFromMe = false
	message.Info.SenderAlt = types.EmptyJID
	if _, ok := eventMetadata(message, allowed, start); ok {
		t.Fatal("unrelated sender passed filter")
	}
	message.Info.Sender = types.NewJID("200", types.HiddenUserServer)
	message.Info.Timestamp = time.Unix(999, 0)
	if _, ok := eventMetadata(message, allowed, start); ok {
		t.Fatal("old queued message passed freshness filter")
	}
	message.Info.Timestamp = time.Unix(1000, 0)
	unavailable := &events.UndecryptableMessage{Info: message.Info, IsUnavailable: true, UnavailableType: events.UnavailableTypeViewOnce}
	record, ok = eventMetadata(unavailable, allowed, start)
	if !ok || !record.Unavailable || !record.ViewOnce || record.HasImage || record.HasURL || record.HasKey {
		t.Fatal("restricted delivery invented a usable photo")
	}
	if image.GetViewOnce() {
		t.Fatal("inspection mutated source media")
	}
}

func TestPrivateDirectoryAndSyntheticQR(t *testing.T) {
	path := filepath.Join(t.TempDir(), "nchat-viewonce-probe-fixture")
	dir, err := preparePrivateDir(path)
	if err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(dir)
	if err != nil || info.Mode().Perm() != 0700 {
		t.Fatal("probe directory is not private")
	}
	if _, err := preparePrivateDir(path); err == nil {
		t.Fatal("existing probe directory was reused")
	}
	for _, unsafe := range []string{".", "/", "/Users/nchat-viewonce-probe-fixture", filepath.Join(t.TempDir(), "wrong-prefix")} {
		if _, err := preparePrivateDir(unsafe); err == nil {
			t.Fatal("unsafe destination accepted")
		}
	}
	pngData, err := writeQR(dir, "synthetic pairing data, not an account QR")
	if err != nil {
		t.Fatal(err)
	}
	config, err := png.DecodeConfig(bytes.NewReader(pngData))
	if err != nil || config.Width != 320 || config.Height != 320 {
		t.Fatal("synthetic QR image is invalid")
	}
	info, err = os.Stat(filepath.Join(dir, "qr.png"))
	if err != nil || info.Mode().Perm() != 0600 {
		t.Fatal("QR file is not private")
	}
	entries, err := os.ReadDir(dir)
	if err != nil || len(entries) != 1 || entries[0].Name() != "qr.png" {
		t.Fatal("temporary QR files were retained")
	}
}

func TestNonceLoopbackQRRoutes(t *testing.T) {
	const token = "0123456789abcdef0123456789abcdef"
	const host = "127.0.0.1:12345"
	state := &qrState{status: "pairing", png: []byte("synthetic-private-QR")}
	handler := qrHandler(token, host, state)
	request := func(method, path, requestHost string) *httptest.ResponseRecorder {
		r := httptest.NewRequest(method, "http://"+host+path, nil)
		r.Host = requestHost
		w := httptest.NewRecorder()
		handler.ServeHTTP(w, r)
		return w
	}
	for _, path := range []string{"/", "/qr.png", "/" + token + "/../session.sqlite", "/" + token + "/session.sqlite"} {
		if w := request("GET", path, host); w.Code != http.StatusNotFound {
			t.Fatal("arbitrary route can expose private files")
		}
	}
	if w := request("POST", "/"+token+"/", host); w.Code != 404 {
		t.Fatal("QR server accepts uploads")
	}
	if w := request("GET", "/"+token+"/qr.png", "foreign.example"); w.Code != 404 {
		t.Fatal("foreign host accepted")
	}
	if w := request("GET", "/"+token+"/qr.png", host); w.Code != 200 || w.Body.String() != "synthetic-private-QR" || w.Header().Get("Cache-Control") != "no-store" || w.Header().Get("Referrer-Policy") != "no-referrer" {
		t.Fatal("private QR route/cache policy incorrect")
	}
	if w := request("GET", "/"+token+"/", host); w.Code != 200 || strings.Contains(w.Body.String(), "synthetic-private-QR") || !strings.Contains(w.Body.String(), "setInterval") {
		t.Fatal("QR page embeds secrets or does not refresh")
	}
	state.set("connected", nil)
	if w := request("GET", "/"+token+"/qr.png", host); w.Code != 404 {
		t.Fatal("paired QR remains accessible")
	}
	if w := request("GET", "/"+token+"/status", host); strings.TrimSpace(w.Body.String()) != `{"status":"connected"}` {
		t.Fatal("status endpoint exposed non-generic data")
	}
}

func TestArgumentsAreBoundedAndNeverEchoIdentity(t *testing.T) {
	o, err := parseOptions([]string{"--probe-dir", "/tmp/nchat-viewonce-probe-synthetic", "--sender", "100@s.whatsapp.net"})
	if err != nil || o.lifetime != 20*time.Minute {
		t.Fatal("valid options rejected")
	}
	for _, mode := range []string{"web", "android"} {
		control, err := parseOptions([]string{"--probe-dir", "/tmp/nchat-viewonce-probe-synthetic", "--login-control", mode})
		if err != nil || control.loginControl != mode || len(control.senders) != 0 {
			t.Fatal("login-only control requires no private sender identity")
		}
	}
	if _, err := parseOptions([]string{"--probe-dir", "/tmp/nchat-viewonce-probe-synthetic", "--login-control", "private-invalid-mode"}); err == nil || strings.Contains(err.Error(), "private-invalid-mode") {
		t.Fatal("invalid login mode accepted or echoed")
	}
	for _, args := range [][]string{{"--sender", "private-identity"}, {"--probe-dir", "/tmp/nchat-viewonce-probe-synthetic", "--sender", "private-identity", "--lifetime", "24h"}, {"--unexpected-private-identity"}} {
		_, err := parseOptions(args)
		if err == nil || strings.Contains(err.Error(), "private-identity") {
			t.Fatal("invalid options unbounded or leaked arguments")
		}
	}
	if _, err := allowedSenders([]string{"private-identity"}); err == nil || strings.Contains(err.Error(), "private-identity") {
		t.Fatal("sender validation leaked its value")
	}
	if _, err := allowedSenders([]string{"100@s.whatsapp.net@untrusted"}); err == nil {
		t.Fatal("malformed multi-@ alias was silently truncated")
	}
}
