// viewonce-probe is an isolated delivery-metadata experiment, not a media viewer.
// It never opens an existing profile, requests history, downloads user media,
// marks messages read/played, sends presence, or prints message contents/keys.
// Ordinary whatsmeow linking/app-state/delivery traffic still occurs, including
// its built-in unavailable-message request to the user's primary phone.
package main

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	_ "github.com/mattn/go-sqlite3"
	"github.com/skip2/go-qrcode"
	signalLog "go.mau.fi/libsignal/logger"
	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waCompanionReg"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/proto/waWa6"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/store/sqlstore"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	waLog "go.mau.fi/whatsmeow/util/log"
	"google.golang.org/protobuf/proto"
)

type aliases []string

func (a *aliases) String() string         { return "" } // Never echo supplied identities in help/errors.
func (a *aliases) Set(value string) error { *a = append(*a, value); return nil }

type options struct {
	dir          string
	senders      aliases
	lifetime     time.Duration
	loginControl bool
}

func parseOptions(args []string) (options, error) {
	var o options
	fs := flag.NewFlagSet("viewonce-probe", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	fs.StringVar(&o.dir, "probe-dir", "", "New private nchat-viewonce-probe-* directory under the system temporary directory")
	fs.Var(&o.senders, "sender", "Allowed sender JID; repeat for phone/LID aliases")
	fs.DurationVar(&o.lifetime, "lifetime", 20*time.Minute, "Bounded experiment lifetime")
	fs.BoolVar(&o.loginControl, "login-control", false, "Standard web companion login only; disconnect immediately after authentication")
	if fs.Parse(args) != nil || fs.NArg() != 0 || o.dir == "" || (!o.loginControl && len(o.senders) == 0) || o.lifetime < time.Minute || o.lifetime > time.Hour {
		return options{}, errors.New("use --probe-dir NEW-ABSOLUTE-TEMP-PATH (--sender JID [--sender ALIAS] | --login-control) [--lifetime 20m]")
	}
	return o, nil
}

func preparePrivateDir(value string) (string, error) {
	path := filepath.Clean(value)
	if !filepath.IsAbs(path) || !strings.HasPrefix(filepath.Base(path), "nchat-viewonce-probe-") {
		return "", errors.New("probe directory must be a new absolute nchat-viewonce-probe-* temporary path")
	}
	parent, err := filepath.EvalSymlinks(filepath.Dir(path))
	if err != nil {
		return "", errors.New("probe directory parent must already exist")
	}
	allowed := false
	for _, base := range []string{os.TempDir(), "/tmp"} {
		root, err := filepath.EvalSymlinks(base)
		if err == nil {
			relative, relErr := filepath.Rel(root, parent)
			if relErr == nil && relative != ".." && !strings.HasPrefix(relative, ".."+string(os.PathSeparator)) {
				allowed = true
			}
		}
	}
	if !allowed {
		return "", errors.New("probe directory must be inside a system temporary directory, never an nchat profile")
	}
	path = filepath.Join(parent, filepath.Base(path))
	if err := os.Mkdir(path, 0700); err != nil {
		return "", errors.New("probe directory must not already exist; nothing was overwritten")
	}
	if os.Chmod(path, 0700) != nil {
		return "", errors.New("could not secure new probe directory")
	}
	return path, nil
}

func allowedSenders(raw []string) (map[string]bool, error) {
	result := make(map[string]bool)
	for _, value := range raw {
		jid, err := types.ParseJID(strings.TrimSpace(value))
		if err != nil || strings.Count(value, "@") != 1 || jid.User == "" || (jid.Server != types.DefaultUserServer && jid.Server != types.HiddenUserServer) {
			return nil, errors.New("invalid sender alias")
		}
		result[jid.ToNonAD().String()] = true
	}
	return result, nil
}

func matchesSender(info types.MessageInfo, allowed map[string]bool) bool {
	if info.IsFromMe || info.IsGroup || (info.Chat.Server != types.DefaultUserServer && info.Chat.Server != types.HiddenUserServer) {
		return false
	}
	return allowed[info.Sender.ToNonAD().String()] || allowed[info.SenderAlt.ToNonAD().String()]
}

type metadata struct {
	Timestamp      string `json:"timestamp"`
	Unavailable    bool   `json:"unavailable"`
	ViewOnce       bool   `json:"viewOnce"`
	HasImage       bool   `json:"hasImage"`
	HasURL         bool   `json:"hasURL"`
	HasPath        bool   `json:"hasPath"`
	HasKey         bool   `json:"hasKey"`
	HasPlainHash   bool   `json:"hasPlainHash"`
	HasEncodedHash bool   `json:"hasEncodedHash"`
}

func eventMetadata(event any, allowed map[string]bool, notBefore time.Time) (metadata, bool) {
	var info types.MessageInfo
	var message *waE2E.Message
	var unavailable, once bool
	switch event := event.(type) {
	case *events.Message:
		info, message, once = event.Info, event.Message, event.IsViewOnce
	case *events.UndecryptableMessage:
		info, unavailable, once = event.Info, event.IsUnavailable, event.UnavailableType == events.UnavailableTypeViewOnce
	default:
		return metadata{}, false
	}
	// WhatsApp timestamps have second precision. Reject old queued deliveries,
	// while retaining fresh test messages sent in the probe's starting second.
	if !matchesSender(info, allowed) || info.Timestamp.Unix() < notBefore.Unix() {
		return metadata{}, false
	}
	for depth := 0; message != nil && depth < 12; depth++ {
		var inner *waE2E.Message
		switch {
		case message.GetViewOnceMessage().GetMessage() != nil:
			inner, once = message.GetViewOnceMessage().GetMessage(), true
		case message.GetViewOnceMessageV2().GetMessage() != nil:
			inner, once = message.GetViewOnceMessageV2().GetMessage(), true
		case message.GetViewOnceMessageV2Extension().GetMessage() != nil:
			inner, once = message.GetViewOnceMessageV2Extension().GetMessage(), true
		case message.GetEphemeralMessage().GetMessage() != nil:
			inner = message.GetEphemeralMessage().GetMessage()
		case message.GetDeviceSentMessage().GetMessage() != nil:
			inner = message.GetDeviceSentMessage().GetMessage()
		}
		if inner == nil {
			break
		}
		message = inner
	}
	result := metadata{Timestamp: info.Timestamp.UTC().Format(time.RFC3339Nano), Unavailable: unavailable, ViewOnce: once}
	var media whatsmeow.DownloadableMessage
	switch {
	case message.GetImageMessage() != nil:
		media, result.HasImage = message.GetImageMessage(), true
	case message.GetVideoMessage() != nil:
		media = message.GetVideoMessage()
	case message.GetPtvMessage() != nil:
		media = message.GetPtvMessage()
	case message.GetAudioMessage() != nil:
		media = message.GetAudioMessage()
	}
	if media != nil {
		// Correct lengths are necessary, not proof that media can be downloaded.
		result.HasPath, result.HasKey = media.GetDirectPath() != "", len(media.GetMediaKey()) == 32
		result.HasPlainHash, result.HasEncodedHash = len(media.GetFileSHA256()) == 32, len(media.GetFileEncSHA256()) == 32
		if url, ok := media.(interface{ GetURL() string }); ok {
			result.HasURL = url.GetURL() != ""
		} else if url, ok := media.(whatsmeow.DownloadableMessageWithURL); ok {
			result.HasURL = url.GetUrl() != ""
		}
		if viewOnce, ok := media.(interface{ GetViewOnce() bool }); ok {
			result.ViewOnce = once || viewOnce.GetViewOnce()
		}
	}
	return result, true
}

type discardSignalLog struct{}

func (discardSignalLog) Debug(string, string)   {}
func (discardSignalLog) Info(string, string)    {}
func (discardSignalLog) Warning(string, string) {}
func (discardSignalLog) Error(string, string)   {}
func (discardSignalLog) Configure(string)       {}

func configureClient(device *store.Device, loginControl bool) *whatsmeow.Client {
	var signalLogger signalLog.Loggable = discardSignalLog{}
	signalLog.Setup(&signalLogger)
	store.BaseClientPayload = proto.Clone(store.BaseClientPayload).(*waWa6.ClientPayload)
	store.BaseClientPayload.UserAgent.Platform = waWa6.ClientPayload_UserAgent_ANDROID.Enum()
	store.BaseClientPayload.WebInfo = nil
	store.DeviceProps = proto.Clone(store.DeviceProps).(*waCompanionReg.DeviceProps)
	store.DeviceProps.Os = proto.String("nchat isolated delivery test")
	store.DeviceProps.PlatformType = waCompanionReg.DeviceProps_ANDROID_PHONE.Enum()
	if loginControl {
		store.BaseClientPayload.UserAgent.Platform = waWa6.ClientPayload_UserAgent_WEB.Enum()
		store.BaseClientPayload.WebInfo = &waWa6.ClientPayload_WebInfo{WebSubPlatform: waWa6.ClientPayload_WebInfo_WEB_BROWSER.Enum()}
		store.DeviceProps.PlatformType = waCompanionReg.DeviceProps_CHROME.Enum()
		store.DeviceProps.Os = proto.String("nchat isolated login control")
	}
	store.DeviceProps.RequireFullSync = proto.Bool(false)
	store.DeviceProps.HistorySyncConfig = &waCompanionReg.DeviceProps_HistorySyncConfig{}
	client := whatsmeow.NewClient(device, waLog.Noop)
	client.QRClientType = whatsmeow.PairClientChrome
	client.ManualHistorySyncDownload, client.DisableManualHistorySyncReceipt = true, true
	client.AutomaticMessageRerequestFromPhone, client.EnableDecryptedEventBuffer = false, false
	client.UseRetryMessageStore = false
	// Normal reconnect is needed to complete QR pairing. The parent context
	// bounds it to this experiment's lifetime; stopping never logs out a device.
	return client
}

// Only fixed categories/numeric codes escape this boundary, never raw protocol
// events or errors, which may contain account identities and authentication data.
func connectionDiagnostic(event any) string {
	switch event := event.(type) {
	case *events.PairSuccess:
		return "pair-success"
	case *events.PairError:
		return "pair-error"
	case *events.ConnectFailure:
		return "connect-failure " + strconv.Itoa(int(event.Reason))
	case *events.LoggedOut:
		return "logged-out " + strconv.Itoa(int(event.Reason))
	case *events.TemporaryBan:
		return "temporary-ban " + strconv.Itoa(int(event.Code))
	case *events.ClientOutdated:
		return "client-outdated"
	case *events.QRScannedWithoutMultidevice:
		return "scanned-without-multidevice"
	case *events.StreamError:
		if code, err := strconv.Atoi(event.Code); err == nil && code >= 0 && code <= 999 {
			return "stream-error " + strconv.Itoa(code)
		}
		return "stream-error"
	}
	return ""
}

type qrState struct {
	sync.RWMutex
	png    []byte
	status string
}

func (s *qrState) set(status string, png []byte) {
	s.Lock()
	defer s.Unlock()
	s.status, s.png = status, png
}

func qrHandler(token, host string, state *qrState) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "no-referrer")
		if r.Host != host || (r.Method != http.MethodGet && r.Method != http.MethodHead) {
			http.NotFound(w, r)
			return
		}
		state.RLock()
		status, png := state.status, state.png
		state.RUnlock()
		switch r.URL.Path {
		case "/" + token + "/":
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			w.Header().Set("Content-Security-Policy", "default-src 'none'; img-src 'self'; script-src 'unsafe-inline'; connect-src 'self'")
			io.WriteString(w, `<!doctype html><meta name="referrer" content="no-referrer"><title>nchat isolated delivery test</title><h1>Temporary delivery test</h1><p>Scan in WhatsApp → Linked devices. This does not open any photo.</p><p id="status">Preparing QR…</p><img id="qr" width="320" height="320" alt="Temporary WhatsApp pairing QR"><script>const q=document.getElementById('qr'),s=document.getElementById('status');async function refresh(){try{const r=await fetch('status',{cache:'no-store'}),v=await r.json();s.textContent=v.status;q.hidden=v.status!=='pairing';if(!q.hidden)q.src='qr.png?t='+Date.now()}catch(e){s.textContent='Test stopped';q.hidden=true}}refresh();setInterval(refresh,2000)</script>`)
		case "/" + token + "/qr.png":
			if status != "pairing" || len(png) == 0 {
				http.NotFound(w, r)
				return
			}
			w.Header().Set("Content-Type", "image/png")
			w.Write(png)
		case "/" + token + "/status":
			w.Header().Set("Content-Type", "application/json")
			json.NewEncoder(w).Encode(struct {
				Status string `json:"status"`
			}{status})
		default:
			http.NotFound(w, r)
		}
	})
}

func writeQR(dir, code string) ([]byte, error) {
	png, err := qrcode.Encode(code, qrcode.Medium, 320)
	if err != nil {
		return nil, errors.New("could not encode pairing QR")
	}
	file, err := os.CreateTemp(dir, ".qr-*")
	if err != nil {
		return nil, errors.New("could not create private QR")
	}
	name := file.Name()
	defer os.Remove(name)
	if file.Chmod(0600) != nil {
		file.Close()
		return nil, errors.New("could not secure private QR")
	}
	_, err = file.Write(png)
	closeErr := file.Close()
	if err != nil || closeErr != nil || os.Rename(name, filepath.Join(dir, "qr.png")) != nil {
		return nil, errors.New("could not save private QR")
	}
	return png, nil
}

func run(ctx context.Context, o options, output, statusOutput io.Writer) error {
	allowed, err := allowedSenders(o.senders)
	if err != nil {
		return err
	}
	dir, err := preparePrivateDir(o.dir)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(ctx, o.lifetime)
	defer cancel()
	dbPath := filepath.Join(dir, "session.sqlite")
	file, err := os.OpenFile(dbPath, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return errors.New("could not create isolated session database")
	}
	if file.Close() != nil {
		return errors.New("could not close isolated session database")
	}
	dbURL := (&url.URL{Scheme: "file", Path: dbPath}).String() + "?_foreign_keys=on"
	db, err := sql.Open("sqlite3", dbURL)
	if err != nil {
		return errors.New("could not open isolated session database")
	}
	container := sqlstore.NewWithDB(db, "sqlite3", waLog.Noop)
	defer func() { cancel(); container.Close() }()
	if container.Upgrade(ctx) != nil {
		return errors.New("could not initialize isolated session database")
	}
	client := configureClient(container.NewDevice(), o.loginControl)
	client.BackgroundEventCtx = ctx
	defer func() { cancel(); client.Disconnect() }()
	state := &qrState{status: "pairing"}
	defer func() { state.set("stopped", nil); os.Remove(filepath.Join(dir, "qr.png")) }()
	var tokenBytes [16]byte
	if _, err = rand.Read(tokenBytes[:]); err != nil {
		return errors.New("could not create private QR address")
	}
	token := hex.EncodeToString(tokenBytes[:])
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return errors.New("could not start loopback QR page")
	}
	server := &http.Server{Handler: qrHandler(token, listener.Addr().String(), state), ReadHeaderTimeout: 5 * time.Second, WriteTimeout: 5 * time.Second, MaxHeaderBytes: 8192, ErrorLog: log.New(io.Discard, "", 0)}
	go server.Serve(listener)
	defer func() { cancel(); server.Close() }()
	fmt.Fprintln(statusOutput, "QR page: http://"+listener.Addr().String()+"/"+token+"/")
	var outputMu sync.Mutex
	controlConnected := make(chan struct{}, 1)
	notBefore := time.Now().UTC()
	writeStatus := func(value string) { outputMu.Lock(); defer outputMu.Unlock(); fmt.Fprintln(statusOutput, value) }
	client.AddEventHandler(func(event any) {
		if category := connectionDiagnostic(event); category != "" {
			writeStatus("Connection event: " + category)
		}
		switch event.(type) {
		case *events.Connected:
			state.set("connected", nil)
			os.Remove(filepath.Join(dir, "qr.png"))
			writeStatus("Connected")
			if o.loginControl {
				select {
				case controlConnected <- struct{}{}:
				default:
				}
			}
		case *events.Disconnected:
			state.set("disconnected", nil)
			writeStatus("Disconnected")
		}
		if o.loginControl {
			return
		}
		if record, ok := eventMetadata(event, allowed, notBefore); ok {
			outputMu.Lock()
			json.NewEncoder(output).Encode(record)
			outputMu.Unlock()
		}
	})
	qr, err := client.GetQRChannel(ctx)
	if err != nil {
		return errors.New("could not prepare isolated pairing")
	}
	if client.ConnectContext(ctx) != nil {
		return errors.New("isolated probe connection failed")
	}
	for {
		select {
		case <-controlConnected:
			writeStatus("Standard login control passed; disconnecting isolated test")
			return nil
		case <-ctx.Done():
			return nil
		case item, ok := <-qr:
			if !ok {
				qr = nil
				continue
			}
			switch item.Event {
			case whatsmeow.QRChannelEventCode:
				png, err := writeQR(dir, item.Code)
				if err != nil {
					return err
				}
				state.set("pairing", png)
				writeStatus("QR ready")
			case "success":
				state.set("paired", nil)
				os.Remove(filepath.Join(dir, "qr.png"))
				writeStatus("Paired")
			default:
				// QR channel errors themselves can contain private protocol data.
				switch item.Event {
				case "timeout", "error", "err-unexpected-state", "err-client-outdated", "err-scanned-without-multidevice", "passkey-request", "passkey-confirmation":
					writeStatus("Pairing status: " + item.Event)
				default:
					writeStatus("Pairing status: unknown")
				}
				return errors.New("isolated pairing did not complete")
			}
		}
	}
}

func main() {
	o, err := parseOptions(os.Args[1:])
	if err == nil {
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		err = run(ctx, o, os.Stdout, os.Stderr)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
