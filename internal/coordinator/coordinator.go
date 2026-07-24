package coordinator

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	maxRequestBytes = 64 * 1024
	maxReportedMods = 64
	maxPlayers      = 128
)

type Action string

const (
	ActionAllow   Action = "allow"
	ActionKick    Action = "kick"
	ActionObserve Action = "observe"
	ActionWait    Action = "wait"
)

type AllowedMod struct {
	Version string `json:"version"`
	Digest  string `json:"digest"`
}

type ReportedMod struct {
	ID      string `json:"id"`
	Version string `json:"version"`
	Digest  string `json:"digest"`
}

type Report struct {
	ServerID        string        `json:"serverId"`
	UserID          string        `json:"userId"`
	ProtocolVersion string        `json:"protocolVersion"`
	Challenge       string        `json:"challenge"`
	Sequence        uint64        `json:"sequence"`
	SentAt          time.Time     `json:"sentAt"`
	Mods            []ReportedMod `json:"mods"`
	Violations      []string      `json:"violations"`
}

type OnlinePlayer struct {
	UserID string `json:"userId"`
	Name   string `json:"name"`
}

type Decision struct {
	UserID string   `json:"userId"`
	Name   string   `json:"name"`
	Action Action   `json:"action"`
	Reason string   `json:"reason,omitempty"`
	Mods   []string `json:"mods,omitempty"`
}

type LauncherManifest struct {
	LauncherVersion         string `json:"launcherVersion"`
	MinimumLauncherVersion  string `json:"minimumLauncherVersion"`
	LauncherDownloadURL     string `json:"launcherDownloadUrl"`
	LauncherSHA256          string `json:"launcherSha256"`
	PalVerifyVersion        string `json:"palVerifyVersion"`
	PalVerifyPackageDigest  string `json:"palVerifyPackageDigest"`
	RequiredPalworldBuildID string `json:"requiredPalworldBuildId"`
	PalworldVersion         string `json:"palworldVersion"`
	ServerOnline            bool   `json:"serverOnline"`
	ServerAddress           string `json:"serverAddress"`
	WebsiteURL              string `json:"websiteUrl"`
	NewsURL                 string `json:"newsUrl"`
}

type Config struct {
	GracePeriod      time.Duration
	ReportMaxAge     time.Duration
	ChallengeMaxAge  time.Duration
	AllowedMods      map[string]AllowedMod
	LauncherManifest LauncherManifest
}

func (store *Store) LauncherManifest() LauncherManifest {
	store.mu.Lock()
	defer store.mu.Unlock()
	return store.config.LauncherManifest
}

func (store *Store) AllowedMod(id string) AllowedMod {
	store.mu.Lock()
	defer store.mu.Unlock()
	return store.config.AllowedMods[id]
}

type sessionChallenge struct {
	value     string
	expiresAt time.Time
}

type Store struct {
	mu         sync.Mutex
	config     Config
	reports    map[string]Report
	sequences  map[string]uint64
	firstSeen  map[string]time.Time
	activeSeen map[string]time.Time
	challenges map[string]sessionChallenge
}

func NewStore(config Config) *Store {
	if config.ChallengeMaxAge <= 0 {
		config.ChallengeMaxAge = 15 * time.Second
	}
	return &Store{
		config:     config,
		reports:    make(map[string]Report),
		sequences:  make(map[string]uint64),
		firstSeen:  make(map[string]time.Time),
		activeSeen: make(map[string]time.Time),
		challenges: make(map[string]sessionChallenge),
	}
}

func (store *Store) IssueChallenge(
	serverID string,
	userID string,
	now time.Time,
) (string, error) {
	store.mu.Lock()
	defer store.mu.Unlock()

	serverID = strings.TrimSpace(serverID)
	userID = strings.TrimSpace(userID)
	if !validCompactValue(serverID, 64) ||
		!strings.HasPrefix(strings.ToLower(userID), "steam_") ||
		!validCompactValue(userID, 128) {
		return "", errors.New("invalid challenge identity")
	}
	key := serverID + "\x00" + userID
	lastSeen, active := store.activeSeen[key]
	if !active || now.Sub(lastSeen) > store.config.ChallengeMaxAge {
		return "", errors.New("player session is not active")
	}
	if existing, ok := store.challenges[key]; ok &&
		now.Before(existing.expiresAt) {
		return existing.value, nil
	}

	random := make([]byte, 32)
	if _, err := rand.Read(random); err != nil {
		return "", errors.New("challenge generation failed")
	}
	value := hex.EncodeToString(random)
	store.challenges[key] = sessionChallenge{
		value:     value,
		expiresAt: now.Add(store.config.ChallengeMaxAge),
	}
	return value, nil
}

func (store *Store) AcceptReport(report Report, now time.Time) error {
	store.mu.Lock()
	defer store.mu.Unlock()

	report.ServerID = strings.TrimSpace(report.ServerID)
	report.UserID = strings.TrimSpace(report.UserID)
	report.Challenge = strings.TrimSpace(report.Challenge)
	if !validCompactValue(report.ServerID, 64) {
		return errors.New("invalid server id")
	}
	if !strings.HasPrefix(strings.ToLower(report.UserID), "steam_") {
		return errors.New("unsupported user id")
	}
	if report.ProtocolVersion != "3" {
		return errors.New("unsupported protocol version")
	}
	key := report.ServerID + "\x00" + report.UserID
	challenge, issued := store.challenges[key]
	if !issued || now.After(challenge.expiresAt) ||
		report.Challenge != challenge.value {
		return errors.New("invalid or expired challenge")
	}
	if report.Sequence == 0 || report.Sequence <= store.sequences[report.UserID] {
		return errors.New("replayed sequence")
	}
	if len(report.Mods) > maxReportedMods {
		return errors.New("mod inventory is too large")
	}
	for _, mod := range report.Mods {
		if !validCompactValue(mod.ID, 96) ||
			!validCompactValue(mod.Version, 48) ||
			!validDigest(mod.Digest) {
			return errors.New("invalid mod descriptor")
		}
	}
	if len(report.Violations) > 16 {
		return errors.New("too many integrity violations")
	}
	for _, violation := range report.Violations {
		if !validCompactValue(violation, 64) {
			return errors.New("invalid integrity violation")
		}
	}
	if report.SentAt.IsZero() {
		return errors.New("missing report timestamp")
	}

	delete(store.challenges, key)
	store.reports[report.UserID] = report
	store.sequences[report.UserID] = report.Sequence
	return nil
}

func (store *Store) Evaluate(
	serverID string,
	players []OnlinePlayer,
	now time.Time,
) []Decision {
	store.mu.Lock()
	defer store.mu.Unlock()

	serverID = strings.TrimSpace(serverID)
	onlineKeys := make(map[string]struct{}, len(players))
	decisions := make([]Decision, 0, len(players))
	for _, player := range players {
		player.UserID = strings.TrimSpace(player.UserID)
		player.Name = strings.TrimSpace(player.Name)
		key := serverID + "\x00" + player.UserID
		onlineKeys[key] = struct{}{}
		store.activeSeen[key] = now

		if !strings.HasPrefix(strings.ToLower(player.UserID), "steam_") {
			decisions = append(decisions, Decision{
				UserID: player.UserID,
				Name:   player.Name,
				Action: ActionObserve,
				Reason: "PLATFORM_UNRESOLVED",
			})
			continue
		}

		firstSeen, seen := store.firstSeen[key]
		if !seen {
			firstSeen = now
			store.firstSeen[key] = now
		}

		report, reported := store.reports[player.UserID]
		if !reported ||
			report.SentAt.After(now.Add(30*time.Second)) ||
			now.Sub(report.SentAt) > store.config.ReportMaxAge {
			action := ActionWait
			reason := "VERIFICATION_GRACE"
			if now.Sub(firstSeen) >= store.config.GracePeriod {
				action = ActionKick
				reason = "MISSING_PALVERIFY"
			}
			decisions = append(decisions, Decision{
				UserID: player.UserID,
				Name:   player.Name,
				Action: action,
				Reason: reason,
			})
			continue
		}

		if len(report.Violations) != 0 {
			decisions = append(decisions, Decision{
				UserID: player.UserID,
				Name:   player.Name,
				Action: ActionKick,
				Reason: "INTEGRITY_VIOLATION",
			})
			continue
		}

		rejected := rejectedModIDs(report.Mods, store.config.AllowedMods)
		if len(rejected) != 0 {
			decisions = append(decisions, Decision{
				UserID: player.UserID,
				Name:   player.Name,
				Action: ActionKick,
				Reason: "UNAPPROVED_MOD",
				Mods:   rejected,
			})
			continue
		}

		decisions = append(decisions, Decision{
			UserID: player.UserID,
			Name:   player.Name,
			Action: ActionAllow,
			Reason: "VERIFIED",
		})
	}

	prefix := serverID + "\x00"
	for key := range store.firstSeen {
		if strings.HasPrefix(key, prefix) {
			if _, online := onlineKeys[key]; !online {
				delete(store.firstSeen, key)
				delete(store.activeSeen, key)
				delete(store.challenges, key)
			}
		}
	}
	sort.Slice(decisions, func(left, right int) bool {
		return decisions[left].UserID < decisions[right].UserID
	})
	return decisions
}

func rejectedModIDs(
	reported []ReportedMod,
	allowed map[string]AllowedMod,
) []string {
	rejected := make([]string, 0)
	seen := make(map[string]struct{}, len(reported))
	for _, mod := range reported {
		approved, exists := allowed[mod.ID]
		if _, duplicate := seen[mod.ID]; duplicate {
			rejected = append(rejected, mod.ID)
			continue
		}
		seen[mod.ID] = struct{}{}
		if !exists ||
			mod.Version != approved.Version ||
			!strings.EqualFold(mod.Digest, approved.Digest) {
			rejected = append(rejected, mod.ID)
		}
	}
	for id := range allowed {
		if _, exists := seen[id]; !exists {
			rejected = append(rejected, id)
		}
	}
	sort.Strings(rejected)
	return rejected
}

func validCompactValue(value string, max int) bool {
	value = strings.TrimSpace(value)
	if value == "" || len(value) > max {
		return false
	}
	for _, character := range value {
		if character < 0x20 || character > 0x7e {
			return false
		}
	}
	return true
}

func validDigest(value string) bool {
	if len(value) < 8 || len(value) > 128 {
		return false
	}
	for _, character := range value {
		if (character < '0' || character > '9') &&
			(character < 'a' || character > 'f') &&
			(character < 'A' || character > 'F') &&
			character != '-' {
			return false
		}
	}
	return true
}

type evaluateRequest struct {
	ServerID string         `json:"serverId"`
	SentAt   time.Time      `json:"sentAt"`
	Players  []OnlinePlayer `json:"players"`
}

type evaluateResponse struct {
	Decisions []Decision `json:"decisions"`
}

type challengeRequest struct {
	ServerID string `json:"serverId"`
	UserID   string `json:"userId"`
}

type challengeResponse struct {
	Challenge string `json:"challenge"`
}

func NewHandler(
	store *Store,
	serverToken string,
	now func() time.Time,
	logger *log.Logger,
) http.Handler {
	if now == nil {
		now = time.Now
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", func(
		response http.ResponseWriter,
		_ *http.Request,
	) {
		response.Header().Set("Cache-Control", "no-store")
		response.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(response, `{"status":"ok"}`)
	})
	mux.HandleFunc("GET /v1/launcher/manifest", func(
		response http.ResponseWriter,
		_ *http.Request,
	) {
		response.Header().Set("Cache-Control", "no-store")
		response.Header().Set("X-Content-Type-Options", "nosniff")
		response.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(response).Encode(
			store.LauncherManifest(),
		); err != nil && logger != nil {
			logger.Printf("encode launcher manifest: %v", err)
		}
	})
	mux.HandleFunc("POST /v1/client/challenge", func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		setPrivateHeaders(response)
		if !jsonMediaType(request.Header.Get("Content-Type")) {
			http.Error(
				response,
				"content type must be application/json",
				http.StatusUnsupportedMediaType,
			)
			return
		}
		var input challengeRequest
		if err := decodeLimitedJSON(request, &input); err != nil {
			http.Error(response, "invalid challenge request", http.StatusBadRequest)
			return
		}
		challenge, err := store.IssueChallenge(
			input.ServerID,
			input.UserID,
			now(),
		)
		if err != nil {
			http.Error(response, "active session required", http.StatusConflict)
			return
		}
		response.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(response).Encode(challengeResponse{
			Challenge: challenge,
		}); err != nil && logger != nil {
			logger.Printf("encode challenge response: %v", err)
		}
	})
	mux.HandleFunc("POST /v1/client/report", func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		setPrivateHeaders(response)
		if !jsonMediaType(request.Header.Get("Content-Type")) {
			http.Error(
				response,
				"content type must be application/json",
				http.StatusUnsupportedMediaType,
			)
			return
		}
		var report Report
		if err := decodeLimitedJSON(request, &report); err != nil {
			http.Error(response, "invalid report", http.StatusBadRequest)
			return
		}
		if err := store.AcceptReport(report, now()); err != nil {
			http.Error(response, err.Error(), http.StatusBadRequest)
			return
		}
		if logger != nil {
			modIDs := make([]string, 0, len(report.Mods))
			for _, mod := range report.Mods {
				modIDs = append(modIDs, mod.ID)
			}
			sort.Strings(modIDs)
			logger.Printf(
				"client_report user=%s mods=%s violations=%s",
				compactUserID(report.UserID),
				strings.Join(modIDs, ","),
				strings.Join(report.Violations, ","),
			)
		}
		response.WriteHeader(http.StatusAccepted)
	})
	mux.HandleFunc("POST /v1/server/evaluate", func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		setPrivateHeaders(response)
		if !authorized(request, serverToken) {
			http.Error(response, "unauthorized", http.StatusUnauthorized)
			return
		}
		if !jsonMediaType(request.Header.Get("Content-Type")) {
			http.Error(
				response,
				"content type must be application/json",
				http.StatusUnsupportedMediaType,
			)
			return
		}
		var input evaluateRequest
		if err := decodeLimitedJSON(request, &input); err != nil ||
			!validCompactValue(input.ServerID, 64) ||
			len(input.Players) > maxPlayers {
			http.Error(response, "invalid evaluation", http.StatusBadRequest)
			return
		}
		for _, player := range input.Players {
			if !validCompactValue(player.UserID, 128) ||
				!validCompactValue(player.Name, 96) {
				http.Error(
					response,
					"invalid player descriptor",
					http.StatusBadRequest,
				)
				return
			}
		}
		decisions := store.Evaluate(input.ServerID, input.Players, now())
		if logger != nil {
			for _, decision := range decisions {
				if decision.Action == ActionKick {
					logger.Printf(
						"kick_decision user=%s reason=%s mods=%s",
						compactUserID(decision.UserID),
						decision.Reason,
						strings.Join(decision.Mods, ","),
					)
				}
			}
		}
		response.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(response).Encode(evaluateResponse{
			Decisions: decisions,
		}); err != nil && logger != nil {
			logger.Printf("encode response: %v", err)
		}
	})
	return mux
}

func authorized(request *http.Request, token string) bool {
	if token == "" {
		return false
	}
	return request.Header.Get("Authorization") == "Bearer "+token
}

func jsonMediaType(contentType string) bool {
	contentType = strings.ToLower(strings.TrimSpace(contentType))
	return contentType == "application/json" ||
		strings.HasPrefix(contentType, "application/json;")
}

func decodeLimitedJSON(request *http.Request, target any) error {
	if request.ContentLength > maxRequestBytes {
		return errors.New("request is too large")
	}
	reader := http.MaxBytesReader(nil, request.Body, maxRequestBytes)
	decoder := json.NewDecoder(reader)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return err
	}
	var extra any
	if err := decoder.Decode(&extra); !errors.Is(err, io.EOF) {
		return errors.New("request contains trailing JSON")
	}
	return nil
}

func setPrivateHeaders(response http.ResponseWriter) {
	response.Header().Set("Cache-Control", "private, no-store")
	response.Header().Set("X-Content-Type-Options", "nosniff")
}

func compactUserID(userID string) string {
	if len(userID) <= 12 {
		return userID
	}
	return fmt.Sprintf("%s...%s", userID[:8], userID[len(userID)-4:])
}
