package main

import (
	"encoding/json"
	"strings"
	"testing"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"google.golang.org/protobuf/proto"
)

func TestViewOnceLabelsAndDownloadFlags(t *testing.T) {
	for _, test := range []struct {
		text, want string
		once       bool
	}{
		{"Photo caption", "Photo caption", false},
		{"Photo caption", "[View once] Photo caption", true},
		{"", "[View once]", true},
		{"[Voice note]", "[View once] [Voice note]", true},
		{"[Video note]", "[View once] [Video note]", true},
		{"[View once] Caption", "[View once] Caption", true},
	} {
		if got := viewOnceLabel(test.text, test.once); got != test.want {
			t.Fatalf("label(%q, %t) = %q", test.text, test.once, got)
		}
	}
	for _, test := range []struct {
		media whatsmeow.DownloadableMessage
		once  bool
	}{
		{&waE2E.ImageMessage{}, false},
		{&waE2E.ImageMessage{ViewOnce: proto.Bool(true)}, true},
		{&waE2E.VideoMessage{ViewOnce: proto.Bool(true)}, true},
		{&waE2E.AudioMessage{ViewOnce: proto.Bool(true)}, true},
		{&waE2E.DocumentMessage{}, false},
	} {
		if got := isViewOnceDownload(test.media); got != test.once {
			t.Fatalf("view-once download flag for %T = %t", test.media, got)
		}
	}
}

func TestViewOnceWrapperFlagsRemainRestricted(t *testing.T) {
	for _, original := range []*waE2E.Message{
		{ImageMessage: &waE2E.ImageMessage{Caption: proto.String("Photo")}},
		{VideoMessage: &waE2E.VideoMessage{Caption: proto.String("Video")}},
		{AudioMessage: &waE2E.AudioMessage{PTT: proto.Bool(true)}},
		{PtvMessage: &waE2E.VideoMessage{}},
	} {
		before := proto.Clone(original)
		wrapped := &waE2E.Message{ViewOnceMessageV2: &waE2E.FutureProofMessage{Message: original}}
		unwrapped := unwrapActionMessage(wrapped)
		var media whatsmeow.DownloadableMessage
		switch {
		case unwrapped.ImageMessage != nil:
			media = unwrapped.ImageMessage
		case unwrapped.VideoMessage != nil:
			media = unwrapped.VideoMessage
		case unwrapped.AudioMessage != nil:
			media = unwrapped.AudioMessage
		case unwrapped.PtvMessage != nil:
			media = unwrapped.PtvMessage
		}
		if !isViewOnceDownload(media) {
			t.Fatalf("wrapper-only %T lost its view-once metadata", media)
		}
		if !proto.Equal(before, original) {
			t.Fatal("flag propagation mutated the provider's original message")
		}
		if _, err := forwardedMessage(unwrapped); err == nil {
			t.Fatal("visible label/metadata removed the existing forwarding restriction")
		}
	}
}

func TestViewOnceDownloadInfoBackwardsCompatible(t *testing.T) {
	const legacy = `{"Version_int":2,"Url_string":"https://example.invalid/synthetic-media","DirectPath_string":"","TargetPath_string":"/synthetic/image.jpg","MediaKey_arraybyte":null,"MediaType_MediaType":"","FileEncSha256_arraybyte":null,"FileSha256_arraybyte":null}`
	var info DownloadInfo
	if err := json.Unmarshal([]byte(legacy), &info); err != nil || info.ViewOnce || info.Version != downloadInfoVersion {
		t.Fatalf("legacy download ID changed: %+v, %v", info, err)
	}
	encoded, err := json.Marshal(info)
	if err != nil || string(encoded) != legacy {
		t.Fatalf("ordinary download metadata encoding changed: %s, %v", encoded, err)
	}
	info.ViewOnce = true
	encoded, err = json.Marshal(info)
	if err != nil || !strings.Contains(string(encoded), `"ViewOnce_bool":true`) {
		t.Fatalf("view-once flag did not reach shared bridge metadata: %s, %v", encoded, err)
	}
	var decoded DownloadInfo
	if err := json.Unmarshal(encoded, &decoded); err != nil || !decoded.ViewOnce || decoded.Version != 2 || decoded.Url != info.Url {
		t.Fatalf("additive flag changed existing download fields: %+v, %v", decoded, err)
	}
}
