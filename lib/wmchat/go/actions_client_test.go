package main

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"

	"go.mau.fi/whatsmeow/store"
)

type versionTransport func(*http.Request) (*http.Response, error)

func (f versionTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func TestWhatsAppVersionRefresh(t *testing.T) {
	original := store.GetWAVersion()
	defer store.SetWAVersion(original)
	client := func(body string, status int, offline bool) *http.Client {
		return &http.Client{Transport: versionTransport(func(req *http.Request) (*http.Response, error) {
			if req.URL.Scheme != "https" || req.URL.Host != "web.whatsapp.com" || req.Method != "GET" || req.Body != nil || req.Header.Get("Authorization") != "" {
				t.Fatal("version lookup must only fetch the public official page")
			}
			if offline {
				return nil, fmt.Errorf("offline")
			}
			return &http.Response{StatusCode: status, Body: io.NopCloser(strings.NewReader(body)), Header: make(http.Header)}, nil
		})}
	}
	newer := original
	newer[2]++
	if err := refreshWhatsAppVersion(context.Background(), client(fmt.Sprintf(`{"client_revision":%d,}`, newer[2]), 200, false)); err != nil || store.GetWAVersion() != newer {
		t.Fatalf("newer official version was not applied: %v", err)
	}
	for _, fixture := range []struct {
		body    string
		status  int
		offline bool
		fail    bool
	}{
		{`{"client_revision":1,}`, 200, false, false},
		{`{"client_revision":0,}`, 200, false, true},
		{`no version here`, 200, false, true},
		{`unavailable`, 503, false, true},
		{"", 0, true, true},
	} {
		err := refreshWhatsAppVersion(context.Background(), client(fixture.body, fixture.status, fixture.offline))
		if (err != nil) != fixture.fail || store.GetWAVersion() != newer {
			t.Fatalf("failed/stale lookup changed working version: %v", err)
		}
	}
}
