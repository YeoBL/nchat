// Read-only, target-scoped poll diagnostics. This deliberately has no account,
// protocol, or network dependencies and never emits message text or identities.
// Usage: go run utils/nchat-poll-cache-summary.go --cache EXACTPATH --chat CHATID --message MSGID
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/gob"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"sort"
)

const cacheMagic = "NCHAT-POLLS\n"
const maxCacheBytes = 128 * 1024 * 1024

// Gob matches exported field names structurally: omitted fields are not needed.
type pollState struct {
	Options   []string
	Votes     map[string][]string
	VoteTimes map[string]int64
}

type pendingDTO struct {
	Key, ID, Sender string
	Timestamp       int64
	HasVote         bool
	Message, Vote   []byte
}

type cacheDTO struct {
	Version int
	States  map[string]*pollState
	Pending []pendingDTO
}

type ledgerSummary struct {
	RawIdentities              int `json:"raw_identity_count"`
	ActiveIdentities           int `json:"active_identity_count"`
	RecognizedActiveIdentities int `json:"recognized_active_identity_count"`
	ZeroChoiceIdentities       int `json:"zero_choice_identity_count"`
	UnknownChoiceIdentities    int `json:"unknown_choice_identity_count"`
	UnknownChoices             int `json:"distinct_unknown_choice_count"`
}

type timeSummary struct {
	Count   int    `json:"count"`
	Zero    int    `json:"zero_count"`
	Missing int    `json:"missing_for_ledger_count"`
	Min     *int64 `json:"min_ms"`
	Max     *int64 `json:"max_ms"`
}

type pendingSummary struct {
	Total     int    `json:"total"`
	Encrypted int    `json:"encrypted"`
	Decoded   int    `json:"decoded"`
	Digest    string `json:"semantic_sha256"`
}

type summary struct {
	KeySHA256    string         `json:"selected_key_sha256"`
	Found        bool           `json:"found"`
	Status       string         `json:"status"`
	Version      int            `json:"version"`
	OptionCounts []int          `json:"option_counts"`
	Ledger       ledgerSummary  `json:"ledger"`
	Times        timeSummary    `json:"vote_timestamps"`
	Digest       string         `json:"semantic_sha256"`
	Pending      pendingSummary `json:"pending"`
}

func digest(value any) string {
	encoded, _ := json.Marshal(value)
	sum := sha256.Sum256(encoded)
	return hex.EncodeToString(sum[:])
}

func readCache(path string) (cacheDTO, error) {
	var cache cacheDTO
	before, err := os.Lstat(path)
	if err != nil || !before.Mode().IsRegular() || before.Size() > maxCacheBytes {
		return cache, errors.New("unsafe cache")
	}
	f, err := os.Open(path) // Read-only; no account discovery or file creation.
	if err != nil {
		return cache, errors.New("unreadable cache")
	}
	defer f.Close()
	opened, err := f.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(before, opened) || opened.Size() > maxCacheBytes {
		return cache, errors.New("changed cache")
	}
	data, err := io.ReadAll(io.LimitReader(f, maxCacheBytes+1))
	after, statErr := f.Stat()
	if err != nil || statErr != nil || len(data) > maxCacheBytes || opened.Size() != after.Size() ||
		!opened.ModTime().Equal(after.ModTime()) {
		return cache, errors.New("changed cache")
	}
	versioned := bytes.HasPrefix(data, []byte(cacheMagic))
	if versioned {
		data = data[len(cacheMagic):]
	}
	decoder := gob.NewDecoder(bytes.NewReader(data))
	if versioned {
		err = decoder.Decode(&cache)
		if cache.Version != 1 {
			return cacheDTO{}, errors.New("unsupported cache")
		}
	} else {
		err = decoder.Decode(&cache.States)
	}
	if err != nil || cache.States == nil {
		return cacheDTO{}, errors.New("invalid cache")
	}
	var trailing any
	if decoder.Decode(&trailing) != io.EOF {
		return cacheDTO{}, errors.New("invalid cache")
	}
	return cache, nil
}

func summarize(cache cacheDTO, key string) summary {
	keyHash := sha256.Sum256([]byte(key))
	result := summary{KeySHA256: hex.EncodeToString(keyHash[:]), Version: cache.Version, Status: "not_found", OptionCounts: []int{}}
	state := cache.States[key]
	result.Found = state != nil
	// Store only deterministic semantic values in the digest, never in output.
	type voteRecord struct {
		ID      string
		Choices []string
		HasVote bool
		Time    int64
		HasTime bool
	}
	var records []voteRecord
	optionHashes := []string{}
	if state != nil {
		result.Status = "found"
		result.OptionCounts = make([]int, len(state.Options))
		indices := map[string][]int{}
		for index, option := range state.Options {
			hash := sha256.Sum256([]byte(option))
			optionHash := hex.EncodeToString(hash[:])
			optionHashes = append(optionHashes, optionHash)
			indices[optionHash] = append(indices[optionHash], index)
		}
		ids := map[string]bool{}
		for id := range state.Votes {
			ids[id] = true
		}
		for id := range state.VoteTimes {
			ids[id] = true
		}
		orderedIDs := make([]string, 0, len(ids))
		for id := range ids {
			orderedIDs = append(orderedIDs, id)
		}
		sort.Strings(orderedIDs)
		unknownChoices := map[string]bool{}
		result.Ledger.RawIdentities = len(state.Votes)
		for _, id := range orderedIDs {
			choices, hasVote := state.Votes[id]
			unique := map[string]bool{}
			for _, choice := range choices {
				unique[choice] = true
			}
			choices = make([]string, 0, len(unique))
			recognized, unknown := false, false
			for choice := range unique {
				choices = append(choices, choice)
				if positions := indices[choice]; len(positions) != 0 {
					recognized = true
					for _, index := range positions {
						result.OptionCounts[index]++
					}
				} else {
					unknown, unknownChoices[choice] = true, true
				}
			}
			sort.Strings(choices)
			if hasVote {
				if len(choices) == 0 {
					result.Ledger.ZeroChoiceIdentities++
				} else {
					result.Ledger.ActiveIdentities++
				}
				if recognized {
					result.Ledger.RecognizedActiveIdentities++
				}
				if unknown {
					result.Ledger.UnknownChoiceIdentities++
				}
			}
			timestamp, hasTime := state.VoteTimes[id]
			if hasTime {
				result.Times.Count++
				if timestamp == 0 {
					result.Times.Zero++
				}
				if result.Times.Min == nil || timestamp < *result.Times.Min {
					value := timestamp
					result.Times.Min = &value
				}
				if result.Times.Max == nil || timestamp > *result.Times.Max {
					value := timestamp
					result.Times.Max = &value
				}
			} else if hasVote {
				result.Times.Missing++
			}
			records = append(records, voteRecord{id, choices, hasVote, timestamp, hasTime})
		}
		result.Ledger.UnknownChoices = len(unknownChoices)
	}
	result.Digest = digest(struct {
		Found   bool
		Options []string
		Votes   []voteRecord
	}{result.Found, optionHashes, records})
	// Hash each payload internally, then sort records: persisted queue order is
	// not evidence of a semantic change. Raw protobuf bytes are never decoded.
	var pendingDigests []string
	for _, pending := range cache.Pending {
		if pending.Key != key {
			continue
		}
		result.Pending.Total++
		if pending.HasVote {
			result.Pending.Decoded++
		} else {
			result.Pending.Encrypted++
		}
		pendingDigests = append(pendingDigests, digest(pending))
	}
	sort.Strings(pendingDigests)
	result.Pending.Digest = digest(pendingDigests)
	return result
}

func run(args []string, output io.Writer) int {
	flags := flag.NewFlagSet("poll-cache-summary", flag.ContinueOnError)
	flags.SetOutput(io.Discard) // Flag errors must not echo private argument values.
	cachePath := flags.String("cache", "", "exact poll cache file")
	chat := flags.String("chat", "", "exact chat ID")
	message := flags.String("message", "", "exact message ID")
	selfTest := flags.Bool("self-test", false, "run synthetic fixtures only")
	fail := func() int {
		fmt.Fprintln(output, `{"status":"error","error":"invalid arguments or cache unavailable, unsafe, corrupt or unsupported"}`)
		return 1
	}
	if flags.Parse(args) != nil || flags.NArg() != 0 {
		return fail()
	}
	if *selfTest {
		if *cachePath != "" || *chat != "" || *message != "" || selfCheck() != nil {
			return fail()
		}
		fmt.Fprintln(output, `{"self_test":"pass"}`)
		return 0
	}
	if *cachePath == "" || *chat == "" || *message == "" || bytes.IndexByte([]byte(*chat), 0) >= 0 ||
		bytes.IndexByte([]byte(*message), 0) >= 0 {
		return fail()
	}
	cache, err := readCache(*cachePath)
	if err != nil {
		return fail()
	}
	if json.NewEncoder(output).Encode(summarize(cache, *chat+"\x00"+*message)) != nil {
		return 1
	}
	return 0
}

// The sole write path is this explicit self-test, restricted to its new temp dir.
func selfCheck() error {
	dir, err := os.MkdirTemp("", "nchat-poll-summary-test-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)
	key := "private-chat\x00private-message"
	option := "private-option"
	optionHash := sha256.Sum256([]byte(option))
	known := hex.EncodeToString(optionHash[:])
	state := &pollState{Options: []string{option, "other-option"}, Votes: map[string][]string{
		"private-voter": {known, known}, "withdrawn-voter": {}, "unknown-voter": {"private-unknown-hash"}},
		VoteTimes: map[string]int64{"private-voter": 2000, "withdrawn-voter": 3000, "unknown-voter": 0}}
	cache := cacheDTO{Version: 1, States: map[string]*pollState{key: state, "unrelated": {Options: []string{"not-selected"}}},
		Pending: []pendingDTO{{Key: key, ID: "private-update", Sender: "private-sender", Timestamp: 42, Message: []byte("private-encrypted")},
			{Key: key, ID: "decoded-update", HasVote: true, Vote: []byte("private-protobuf")}, {Key: "unrelated", ID: "ignored"}}}
	writeFixture := func(name string, value any, versioned bool) (string, error) {
		var data bytes.Buffer
		if versioned {
			data.WriteString(cacheMagic)
		}
		if err := gob.NewEncoder(&data).Encode(value); err != nil {
			return "", err
		}
		path := filepath.Join(dir, name)
		return path, os.WriteFile(path, data.Bytes(), 0600)
	}
	path, err := writeFixture("versioned", cache, true)
	if err != nil {
		return err
	}
	before, _ := os.ReadFile(path)
	beforeInfo, _ := os.Stat(path)
	var output bytes.Buffer
	if run([]string{"--cache", path, "--chat", "private-chat", "--message", "private-message"}, &output) != 0 {
		return errors.New("versioned read")
	}
	var result summary
	if json.Unmarshal(output.Bytes(), &result) != nil || !result.Found || result.Version != 1 ||
		!reflect.DeepEqual(result.OptionCounts, []int{1, 0}) || result.Ledger.RawIdentities != 3 ||
		result.Ledger.RecognizedActiveIdentities != 1 || result.Ledger.ZeroChoiceIdentities != 1 ||
		result.Ledger.UnknownChoiceIdentities != 1 || result.Times.Zero != 1 ||
		result.Pending.Total != 2 || result.Pending.Encrypted != 1 || result.Pending.Decoded != 1 {
		return errors.New("counts")
	}
	for _, private := range []string{dir, "private-", "withdrawn-voter", "unknown-voter", "other-option", "decoded-update", "unrelated", known} {
		if bytes.Contains(output.Bytes(), []byte(private)) {
			return errors.New("redaction")
		}
	}
	after, _ := os.ReadFile(path)
	afterInfo, _ := os.Stat(path)
	if !bytes.Equal(before, after) || !beforeInfo.ModTime().Equal(afterInfo.ModTime()) {
		return errors.New("read-only")
	}
	loaded, err := readCache(path)
	if err != nil || !reflect.DeepEqual(summarize(loaded, key), result) {
		return errors.New("determinism")
	}
	loaded.Pending[0], loaded.Pending[1] = loaded.Pending[1], loaded.Pending[0]
	loaded.States["unrelated"].Votes = map[string][]string{"ignored": {known}}
	if !reflect.DeepEqual(summarize(loaded, key), result) || summarize(loaded, "missing").Found ||
		summarize(loaded, "missing").Pending.Total != 0 {
		return errors.New("scoping")
	}
	state.Votes["private-voter"] = []string{}
	if summarize(cache, key).Digest == result.Digest {
		return errors.New("semantic changes")
	}
	legacy, err := writeFixture("legacy", cache.States, false)
	if err != nil {
		return err
	}
	legacyCache, err := readCache(legacy)
	if err != nil || legacyCache.Version != 0 || !summarize(legacyCache, key).Found {
		return errors.New("legacy compatibility")
	}
	// A distinct richer wire schema proves omitted production fields are safe.
	type fullState struct {
		Name      string
		Options   []string
		Votes     map[string][]string
		VoteTimes map[string]int64
		IsRead    bool
	}
	compat, err := writeFixture("structural", struct {
		Version int
		States  map[string]*fullState
		Pending []pendingDTO
	}{1, map[string]*fullState{key: {Name: "secret question", Options: state.Options, Votes: state.Votes, VoteTimes: state.VoteTimes, IsRead: true}}, nil}, true)
	if err != nil {
		return err
	}
	if decoded, err := readCache(compat); err != nil || !summarize(decoded, key).Found {
		return errors.New("structural compatibility")
	}
	unsafe := filepath.Join(dir, "symlink")
	if os.Symlink(path, unsafe) != nil {
		return errors.New("symlink fixture")
	}
	if _, err := readCache(unsafe); err == nil {
		return errors.New("symlink accepted")
	}
	cache.Version = 2
	unsupported, err := writeFixture("unsupported", cache, true)
	if err != nil {
		return err
	}
	corrupt := filepath.Join(dir, "corrupt")
	if err := os.WriteFile(corrupt, []byte("private-corrupt-data"), 0600); err != nil {
		return err
	}
	for _, args := range [][]string{{"--cache", path}, {"--cache", unsupported, "--chat", "private-chat", "--message", "private-message"},
		{"--cache", corrupt, "--chat", "private-chat", "--message", "private-message"}, {"--unknown-private-flag"}} {
		output.Reset()
		if run(args, &output) == 0 || !json.Valid(bytes.TrimSpace(output.Bytes())) || bytes.Contains(output.Bytes(), []byte("private")) {
			return errors.New("error redaction")
		}
	}
	return nil
}

func main() { os.Exit(run(os.Args[1:], os.Stdout)) }
