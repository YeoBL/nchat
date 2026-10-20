package whatsmeow

import (
	"encoding/hex"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/beeper/argo-go/codec"
	"github.com/elliotchance/orderedmap/v3"
	"go.mau.fi/whatsmeow/argo"
	"go.mau.fi/whatsmeow/proto/waWa6"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/types"
	"google.golang.org/protobuf/proto"
)

func publicNewsletterFrame(t testing.TB) []byte {
	t.Helper()
	encoded, err := os.ReadFile("testdata/newsletter-metadata-public.hex")
	if err != nil {
		t.Fatal(err)
	}
	frame, err := hex.DecodeString(strings.Join(strings.Fields(string(encoded)), ""))
	if err != nil {
		t.Fatal(err)
	}
	return frame
}

func TestMexPublicNewsletterMetadata(t *testing.T) {
	frame := publicNewsletterFrame(t)
	for _, query := range []string{queryFetchNewsletter, queryFetchNewsletterDesktop} {
		data, err := decodeMexResponse(query, "argo", frame)
		if err != nil {
			t.Fatal(err)
		}
		var response respGetNewsletterInfo
		if err := json.Unmarshal(data, &response); err != nil {
			t.Fatal(err)
		}
		if response.Newsletter == nil || response.Newsletter.ID.String() != "120363166407666729@newsletter" ||
			response.Newsletter.ThreadMeta.Name.Text != "Page Six" || response.Newsletter.ThreadMeta.Description.Text == "" {
			t.Fatalf("public metadata or nested fields lost: %s", data)
		}
	}
}

func mexMap(values map[string]any) *orderedmap.OrderedMap[string, any] {
	result := orderedmap.NewOrderedMap[string, any]()
	for key, value := range values {
		result.Set(key, value)
	}
	return result
}

func encodeMexFixture(t *testing.T, name string, value any) []byte {
	t.Helper()
	typesByName, err := argo.GetStore()
	if err != nil {
		t.Fatal(err)
	}
	encoder := codec.NewArgoEncoder()
	if err := encoder.ValueToArgoWithType(value, typesByName[name]); err != nil {
		t.Fatal(err)
	}
	frame, err := encoder.GetResult()
	if err != nil {
		t.Fatal(err)
	}
	return frame.Bytes()
}

func TestMexSubscribedArraysAndGraphQLErrors(t *testing.T) {
	channel := mexMap(map[string]any{"id": "123@newsletter", "thread_metadata": mexMap(map[string]any{
		"name": mexMap(map[string]any{"id": "name-1", "text": "Synthetic channel"})})})
	value := mexMap(map[string]any{"data": mexMap(map[string]any{"xwa2_newsletter_subscribed": []any{channel}})})
	frame := encodeMexFixture(t, "NewsletterSubscribed", value)
	for _, query := range []string{querySubscribedNewsletters, querySubscribedNewslettersDesktop} {
		data, err := decodeMexResponse(query, "argo", frame)
		var response respGetSubscribedNewsletters
		if err != nil || json.Unmarshal(data, &response) != nil || len(response.Newsletters) != 1 ||
			response.Newsletters[0].ThreadMeta.Name.Text != "Synthetic channel" {
			t.Fatalf("nested array metadata lost: %s %v", data, err)
		}
	}
	value = mexMap(map[string]any{"data": nil, "errors": []any{mexMap(map[string]any{"message": "Permission denied"})}})
	frame = encodeMexFixture(t, "NewsletterMetadata", value)
	if _, err := decodeMexResponse(queryFetchNewsletterDesktop, "argo", frame); err == nil || !strings.Contains(err.Error(), "Permission denied") {
		t.Fatalf("Argo GraphQL error was ignored: %v", err)
	}
	if _, err := decodeMexResponse("unknown-schema", "argo", frame); err == nil {
		t.Fatal("unknown Argo schema accepted")
	}
	if _, err := decodeMexResponse(queryFetchNewsletter, "json", []byte(`{"errors":[{"message":"Permission denied"}]}`)); err == nil || !strings.Contains(err.Error(), "Permission denied") {
		t.Fatalf("JSON GraphQL error was ignored: %v", err)
	}
	if _, err := decodeMexResponse(queryFetchNewsletter, "json", []byte(`{}`)); err == nil {
		t.Fatal("missing GraphQL envelope accepted as success")
	}
}

func TestMexResponseTruncationAndOperationMapping(t *testing.T) {
	frame := publicNewsletterFrame(t)
	for length := 0; length < len(frame); length++ {
		if _, err := decodeMexResponse(queryFetchNewsletterDesktop, "argo", frame[:length]); err == nil {
			t.Fatalf("truncated public frame accepted at %d/%d bytes", length, len(frame))
		}
	}
	names, err := argo.GetQueryIDToMessageName()
	if err != nil {
		t.Fatal(err)
	}
	for id, expected := range map[string]string{mutationFollowNewsletterDesktop: "NewsletterJoin", mutationUnfollowNewsletterDesktop: "NewsletterLeave", mutationUpdateNewsletterDesktop: "NewsletterMetadataUpdate"} {
		if names[id] != expected {
			t.Fatalf("query %s is %q, not %s", id, names[id], expected)
		}
	}
}

func TestMexSelectsSessionPlatformQueryIDs(t *testing.T) {
	original := store.BaseClientPayload
	store.BaseClientPayload = proto.Clone(original).(*waWa6.ClientPayload)
	t.Cleanup(func() { store.BaseClientPayload = original })
	id := types.NewJID("123", types.DefaultUserServer)
	client := &Client{Store: &store.Device{ID: &id}}
	for _, platform := range []waWa6.ClientPayload_UserAgent_Platform{waWa6.ClientPayload_UserAgent_WEB, waWa6.ClientPayload_UserAgent_MACOS} {
		store.BaseClientPayload.UserAgent.Platform = platform.Enum()
		want := mutationFollowNewsletter
		if platform == waWa6.ClientPayload_UserAgent_MACOS {
			want = mutationFollowNewsletterDesktop
		}
		if actual := convertQueryID(client, mutationFollowNewsletter); actual != want {
			t.Fatalf("platform %s selected query %s, expected %s", platform, actual, want)
		}
	}
}

func TestMexSubscriptionResponseSchemas(t *testing.T) {
	for _, operation := range []struct{ name, id, field string }{
		{"NewsletterJoin", mutationFollowNewsletterDesktop, "xwa2_newsletter_join_v2"},
		{"NewsletterLeave", mutationUnfollowNewsletterDesktop, "xwa2_newsletter_leave_v2"},
		{"NewsletterMute", mutationMuteNewsletterDesktop, "xwa2_newsletter_mute_v2"},
		{"NewsletterUnmute", mutationUnmuteNewsletterDesktop, "xwa2_newsletter_unmute_v2"},
	} {
		value := mexMap(map[string]any{"data": mexMap(map[string]any{operation.field: mexMap(map[string]any{
			"id": "123@newsletter", "newsletter_state": mexMap(map[string]any{"type": "ACTIVE"})})})})
		frame := encodeMexFixture(t, operation.name, value)
		data, err := decodeMexResponse(operation.id, "argo", frame)
		if err != nil || !strings.Contains(string(data), operation.field) || !strings.Contains(string(data), "123@newsletter") {
			t.Fatalf("%s response lost: %s %v", operation.name, data, err)
		}
		if err := validateNewsletterMutation(data, types.NewJID("123", types.NewsletterServer), operation.field); err != nil {
			t.Fatalf("%s confirmation rejected: %v", operation.name, err)
		}
	}
}

func TestMexMutationRequiresCorrectConfirmation(t *testing.T) {
	id := types.NewJID("123", types.NewsletterServer)
	for _, invalid := range []types.JID{types.EmptyJID, types.NewJID("123", types.DefaultUserServer)} {
		if err := validateNewsletterMutation(json.RawMessage(`{"xwa2_newsletter_mute_v2":null}`), invalid, "xwa2_newsletter_mute_v2"); err == nil {
			t.Fatal("invalid mutation target accepted")
		}
	}
	for _, response := range []string{`null`, `{}`, `[]`, `{"xwa2_newsletter_mute_v2":null}`,
		`{"xwa2_newsletter_mute_v2":{}}`, `{"xwa2_newsletter_unmute_v2":{"id":"123@newsletter"}}`,
		`{"xwa2_newsletter_mute_v2":{"id":"456@newsletter"}}`} {
		if err := validateNewsletterMutation(json.RawMessage(response), id, "xwa2_newsletter_mute_v2"); err == nil {
			t.Fatalf("unconfirmed channel mutation reported success: %s", response)
		}
	}
	if err := validateNewsletterMutation(json.RawMessage(`{"xwa2_newsletter_follow":{"id":"123@newsletter"}}`), id,
		"xwa2_newsletter_join_v2", "xwa2_newsletter_follow"); err != nil {
		t.Fatalf("verified legacy follow confirmation rejected: %v", err)
	}
}

func TestMexNullableMetadataTimes(t *testing.T) {
	var metadata types.NewsletterThreadMetadata
	if err := json.Unmarshal([]byte(`{"creation_time":"1700000000","name":{"id":"name","text":"Synthetic","update_time":"1700000000000000"}}`), &metadata); err != nil {
		t.Fatal(err)
	}
	if metadata.CreationTime.IsZero() || metadata.Name.UpdateTime.IsZero() {
		t.Fatal("supplied timestamps disappeared")
	}
	if err := json.Unmarshal([]byte(`{"name":{"text":"Renamed"}}`), &metadata); err != nil || metadata.Name.UpdateTime.IsZero() {
		t.Fatalf("absent timestamp overwrote prior metadata: %v", err)
	}
	if err := json.Unmarshal([]byte(`{"creation_time":null,"name":{"update_time":null}}`), &metadata); err != nil ||
		!metadata.CreationTime.IsZero() || !metadata.Name.UpdateTime.IsZero() || metadata.Name.Text != "Renamed" {
		t.Fatalf("nullable metadata failed: %+v %v", metadata, err)
	}
}

func FuzzMexNewsletterResponse(f *testing.F) {
	f.Add(publicNewsletterFrame(f))
	f.Add([]byte{0, 0})
	f.Fuzz(func(t *testing.T, frame []byte) {
		_, _ = decodeMexResponse(queryFetchNewsletterDesktop, "argo", frame)
	})
}
