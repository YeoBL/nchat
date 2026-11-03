package main

import (
	"context"
	"fmt"
	"net/http"
	"net/url"
	"sync"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store"
)

var whatsAppVersionOnce sync.Once

func refreshWhatsAppVersionOnce(proxy string) {
	// The library's version is process-global. Resolve it before creating any
	// clients, never mutate it while another account is connected.
	whatsAppVersionOnce.Do(func() {
		transport := http.DefaultTransport.(*http.Transport).Clone()
		defer transport.CloseIdleConnections()
		if proxy != "" {
			parsed, err := url.Parse(proxy)
			if err != nil {
				LOG_WARNING("WhatsApp version refresh skipped: invalid proxy")
				return
			}
			transport.Proxy = http.ProxyURL(parsed)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := refreshWhatsAppVersion(ctx, &http.Client{Transport: transport}); err != nil {
			// No account data is sent. Keep the bundled version if offline.
			LOG_WARNING("WhatsApp version refresh unavailable; using bundled version")
		} else {
			LOG_INFO("WhatsApp web client version " + store.GetWAVersion().String())
		}
	})
}

func refreshWhatsAppVersion(ctx context.Context, client *http.Client) error {
	version, err := whatsmeow.GetLatestVersion(ctx, client)
	if err != nil {
		return err
	}
	if version == nil || version[0] != 2 || version[1] != 3000 || version[2] == 0 {
		return fmt.Errorf("unexpected WhatsApp web version")
	}
	if store.GetWAVersion().LessThan(*version) {
		store.SetWAVersion(*version)
	}
	return nil
}
