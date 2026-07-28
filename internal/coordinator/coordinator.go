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
	"unicode"
	"unicode/utf8"
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

type AllowedPackage struct {
	Version string `json:"version"`
	Digest  string `json:"digest"`
}

type AllowedMod struct {
	Version            string           `json:"version"`
	Digest             string           `json:"digest"`
	CompatiblePackages []AllowedPackage `json:"compatiblePackages,omitempty"`
}

type ReportedMod struct {
	ID      string `json:"id"`
	Version string `json:"version"`
	Digest  string `json:"digest"`
}

type IntegrityEvidence struct {
	Rule            string `json:"rule"`
	Source          string `json:"source"`
	FileName        string `json:"fileName,omitempty"`
	SHA256          string `json:"sha256,omitempty"`
	SignerName      string `json:"signerName,omitempty"`
	FileDescription string `json:"fileDescription,omitempty"`
	CompanyName     string `json:"companyName,omitempty"`
	MatchReason     string `json:"matchReason"`
	SignatureValid  bool   `json:"signatureValid"`
}

type Report struct {
	ServerID          string              `json:"serverId"`
	UserID            string              `json:"userId"`
	ProtocolVersion   string              `json:"protocolVersion"`
	Challenge         string              `json:"challenge"`
	Sequence          uint64              `json:"sequence"`
	SentAt            time.Time           `json:"sentAt"`
	Mods              []ReportedMod       `json:"mods"`
	Violations        []string            `json:"violations"`
	ViolationEvidence []IntegrityEvidence `json:"violationEvidence,omitempty"`
}

type ClientPreflight struct {
	ServerID          string              `json:"serverId"`
	ProtocolVersion   string              `json:"protocolVersion"`
	Mods              []ReportedMod       `json:"mods"`
	Violations        []string            `json:"violations"`
	ViolationEvidence []IntegrityEvidence `json:"violationEvidence,omitempty"`
}

type PreflightDecision struct {
	Accepted bool     `json:"accepted"`
	Reason   string   `json:"reason"`
	Detail   string   `json:"detail,omitempty"`
	Mods     []string `json:"mods,omitempty"`
}

type OnlinePlayer struct {
	UserID string `json:"userId"`
	Name   string `json:"name"`
}

type Decision struct {
	UserID     string   `json:"userId"`
	Name       string   `json:"name"`
	Action     Action   `json:"action"`
	Reason     string   `json:"reason,omitempty"`
	Mods       []string `json:"mods,omitempty"`
	Violations []string `json:"violations,omitempty"`
	// Detail carries a compact, operator-facing explanation of why this
	// decision was reached (e.g. "NO_VALID_REPORT", "STALE_REPORT age=16s",
	// "PalVerify:VERSION_MISMATCH"). It never contains file/process
	// inventories, only rule codes and bounded scalar facts.
	Detail string `json:"detail,omitempty"`
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
	if err := validateIntegrityEvidence(
		report.Violations,
		report.ViolationEvidence,
	); err != nil {
		return err
	}
	if report.SentAt.IsZero() {
		return errors.New("missing report timestamp")
	}

	delete(store.challenges, key)
	store.reports[report.UserID] = report
	store.sequences[report.UserID] = report.Sequence
	return nil
}

func (store *Store) EvaluatePreflight(
	input ClientPreflight,
) (PreflightDecision, error) {
	store.mu.Lock()
	defer store.mu.Unlock()

	input.ServerID = strings.TrimSpace(input.ServerID)
	if !validCompactValue(input.ServerID, 64) {
		return PreflightDecision{}, errors.New("invalid server id")
	}
	if input.ProtocolVersion != "3" {
		return PreflightDecision{}, errors.New("unsupported protocol version")
	}
	if len(input.Mods) > maxReportedMods {
		return PreflightDecision{}, errors.New("mod inventory is too large")
	}
	for _, mod := range input.Mods {
		if !validCompactValue(mod.ID, 96) ||
			!validCompactValue(mod.Version, 48) ||
			!validDigest(mod.Digest) {
			return PreflightDecision{}, errors.New("invalid mod descriptor")
		}
	}
	if len(input.Violations) > 16 {
		return PreflightDecision{}, errors.New("too many integrity violations")
	}
	for _, violation := range input.Violations {
		if !validCompactValue(violation, 64) {
			return PreflightDecision{}, errors.New("invalid integrity violation")
		}
	}
	if err := validateIntegrityEvidence(
		input.Violations,
		input.ViolationEvidence,
	); err != nil {
		return PreflightDecision{}, err
	}

	if len(input.Violations) != 0 {
		return PreflightDecision{
			Accepted: false,
			Reason:   "INTEGRITY_VIOLATION",
			Detail: integrityDetail(
				input.Violations,
				input.ViolationEvidence,
			),
		}, nil
	}
	rejected, detail := rejectedModIDs(input.Mods, store.config.AllowedMods)
	if len(rejected) != 0 {
		return PreflightDecision{
			Accepted: false,
			Reason:   "UNAPPROVED_MOD",
			Detail:   detail,
			Mods:     rejected,
		}, nil
	}
	return PreflightDecision{
		Accepted: true,
		Reason:   "VERIFIED",
	}, nil
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
				Detail: reportAbsenceDetail(report, reported, now),
			})
			continue
		}

		if len(report.Violations) != 0 {
			decisions = append(decisions, Decision{
				UserID: player.UserID,
				Name:   player.Name,
				Action: ActionKick,
				Reason: "INTEGRITY_VIOLATION",
				Violations: append(
					[]string(nil),
					report.Violations...,
				),
				Detail: integrityDetail(
					report.Violations,
					report.ViolationEvidence,
				),
			})
			continue
		}

		rejected, ruleDetail := rejectedModIDs(report.Mods, store.config.AllowedMods)
		if len(rejected) != 0 {
			decisions = append(decisions, Decision{
				UserID: player.UserID,
				Name:   player.Name,
				Action: ActionKick,
				Reason: "UNAPPROVED_MOD",
				Mods:   rejected,
				Detail: ruleDetail,
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

// rejectedModIDs returns the sorted list of rejected mod IDs and a compact,
// per-mod detail string naming the exact rule each rejection tripped, so an
// operator reading the logs can tell a version drift apart from a rebuilt
// payload (DIGEST_MISMATCH), an unknown mod, or a mod the client failed to
// report at all. Multiple copies are accepted only when every descriptor
// independently matches an approved exact package.
func rejectedModIDs(
	reported []ReportedMod,
	allowed map[string]AllowedMod,
) ([]string, string) {
	rejected := make([]string, 0)
	rules := make(map[string]string)
	seen := make(map[string]struct{}, len(reported))
	for _, mod := range reported {
		approved, exists := allowed[mod.ID]
		seen[mod.ID] = struct{}{}
		rule := ""
		switch {
		case !exists:
			rule = "NOT_WHITELISTED"
		default:
			rule = packagePolicyRule(mod, approved)
		}
		if rule != "" {
			if _, alreadyRejected := rules[mod.ID]; !alreadyRejected {
				rejected = append(rejected, mod.ID)
			}
			rules[mod.ID] = rule
		}
	}
	for id := range allowed {
		if _, exists := seen[id]; !exists {
			rejected = append(rejected, id)
			rules[id] = "REQUIRED_MOD_MISSING"
		}
	}
	sort.Strings(rejected)
	parts := make([]string, 0, len(rejected))
	for _, id := range rejected {
		parts = append(parts, id+":"+rules[id])
	}
	return rejected, strings.Join(parts, ",")
}

func packagePolicyRule(mod ReportedMod, approved AllowedMod) string {
	candidates := make(
		[]AllowedPackage,
		0,
		1+len(approved.CompatiblePackages),
	)
	candidates = append(candidates, AllowedPackage{
		Version: approved.Version,
		Digest:  approved.Digest,
	})
	candidates = append(candidates, approved.CompatiblePackages...)

	versionMatched := false
	for _, candidate := range candidates {
		if candidate.Version != "*" && mod.Version != candidate.Version {
			continue
		}
		versionMatched = true
		if candidate.Digest == "*" ||
			strings.EqualFold(mod.Digest, candidate.Digest) {
			return ""
		}
	}
	if versionMatched {
		return "DIGEST_MISMATCH"
	}
	return "VERSION_MISMATCH"
}

// reportAbsenceDetail distinguishes a client that never delivered a report from
// one whose report arrived but is too old (or clock-skewed into the future),
// including the observed age so MISSING_PALVERIFY kicks are debuggable.
func reportAbsenceDetail(report Report, reported bool, now time.Time) string {
	if !reported {
		return "NO_VALID_REPORT"
	}
	if report.SentAt.After(now.Add(30 * time.Second)) {
		return fmt.Sprintf(
			"CLOCK_SKEW skew=%s",
			report.SentAt.Sub(now).Round(time.Second),
		)
	}
	return fmt.Sprintf(
		"STALE_REPORT age=%s",
		now.Sub(report.SentAt).Round(time.Second),
	)
}

func validateIntegrityEvidence(
	violations []string,
	evidence []IntegrityEvidence,
) error {
	if len(evidence) > 8 {
		return errors.New("too much integrity evidence")
	}
	rules := make(map[string]struct{}, len(violations))
	for _, violation := range violations {
		rules[violation] = struct{}{}
	}
	for _, item := range evidence {
		if _, exists := rules[item.Rule]; !exists ||
			!validCompactValue(item.Rule, 64) {
			return errors.New("invalid integrity evidence rule")
		}
		if item.Source != "module" &&
			item.Source != "memory" &&
			item.Source != "process" {
			return errors.New("invalid integrity evidence source")
		}
		fileName := strings.TrimSpace(item.FileName)
		if fileName != "" &&
			(!validCompactValue(fileName, 128) ||
				strings.ContainsAny(fileName, `/\:`)) {
			return errors.New("invalid integrity evidence file name")
		}
		if item.Source == "module" && fileName == "" {
			return errors.New("missing integrity evidence file name")
		}
		if item.SHA256 != "" &&
			(len(item.SHA256) != 64 || !validDigest(item.SHA256)) {
			return errors.New("invalid integrity evidence digest")
		}
		if item.SignerName != "" &&
			!validEvidenceText(item.SignerName, 128) {
			return errors.New("invalid integrity evidence signer")
		}
		if item.FileDescription != "" &&
			!validEvidenceText(item.FileDescription, 160) {
			return errors.New("invalid integrity evidence description")
		}
		if item.CompanyName != "" &&
			!validEvidenceText(item.CompanyName, 128) {
			return errors.New("invalid integrity evidence company")
		}
		if !validCompactValue(item.MatchReason, 64) {
			return errors.New("invalid integrity evidence match")
		}
	}
	return nil
}

func validEvidenceText(value string, maximum int) bool {
	value = strings.TrimSpace(value)
	if value == "" ||
		!utf8.ValidString(value) ||
		utf8.RuneCountInString(value) > maximum {
		return false
	}
	for _, character := range value {
		if unicode.IsControl(character) {
			return false
		}
	}
	return true
}

func integrityDetail(
	violations []string,
	evidence []IntegrityEvidence,
) string {
	parts := append([]string(nil), violations...)
	for _, item := range evidence {
		attributes := []string{
			"source=" + safeIntegrityScalar(item.Source),
			"match=" + safeIntegrityScalar(item.MatchReason),
		}
		if item.FileName != "" {
			attributes = append(
				attributes,
				"file="+safeIntegrityScalar(item.FileName),
			)
		}
		if item.SHA256 != "" {
			attributes = append(attributes, "sha256="+item.SHA256)
		}
		if item.SignerName != "" {
			attributes = append(
				attributes,
				"signer="+safeIntegrityScalar(item.SignerName),
			)
		}
		if item.FileDescription != "" {
			attributes = append(
				attributes,
				"description="+safeIntegrityScalar(item.FileDescription),
			)
		}
		if item.CompanyName != "" {
			attributes = append(
				attributes,
				"company="+safeIntegrityScalar(item.CompanyName),
			)
		}
		signature := "invalid"
		if item.SignatureValid {
			signature = "valid"
		}
		attributes = append(attributes, "signature="+signature)
		parts = append(
			parts,
			item.Rule+"["+strings.Join(attributes, ",")+"]",
		)
	}
	return strings.Join(parts, "; ")
}

func safeIntegrityScalar(value string) string {
	replacer := strings.NewReplacer(
		"[", " ",
		"]", " ",
		",", " ",
		";", " ",
		"=", " ",
		"|", " ",
	)
	return strings.Join(strings.Fields(replacer.Replace(value)), " ")
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

func validPlayerName(value string, max int) bool {
	value = strings.TrimSpace(value)
	if value == "" ||
		!utf8.ValidString(value) ||
		utf8.RuneCountInString(value) > max {
		return false
	}
	for _, character := range value {
		if unicode.IsControl(character) {
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
	return NewHandlerWithAudit(
		store,
		serverToken,
		now,
		logger,
		nil,
	)
}

func NewHandlerWithAudit(
	store *Store,
	serverToken string,
	now func() time.Time,
	logger *log.Logger,
	audit AuditSink,
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
			if logger != nil {
				logger.Printf(
					"challenge_rejected user=%s reason=%q",
					compactUserID(strings.TrimSpace(input.UserID)),
					err.Error(),
				)
			}
			if audit != nil {
				audit.Emit(request.Context(), AuditEvent{
					OccurredAt: now().UTC(),
					ServerID:   input.ServerID,
					PlayerName: "unknown",
					PlayerRef: auditPlayerReference(
						serverToken,
						input.UserID,
					),
					Action: "CHALLENGE_REJECTED",
					Reason: err.Error(),
				})
			}
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
	mux.HandleFunc("POST /v1/client/preflight", func(
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
		var input ClientPreflight
		if err := decodeLimitedJSON(request, &input); err != nil {
			http.Error(response, "invalid preflight", http.StatusBadRequest)
			return
		}
		decision, err := store.EvaluatePreflight(input)
		if err != nil {
			if logger != nil {
				logger.Printf("preflight_invalid reason=%q", err.Error())
			}
			if audit != nil {
				audit.Emit(request.Context(), AuditEvent{
					OccurredAt: now().UTC(),
					ServerID:   input.ServerID,
					PlayerName: "unknown",
					PlayerRef:  "unavailable",
					Action:     "PREFLIGHT_INVALID",
					Reason:     err.Error(),
				})
			}
			http.Error(response, "invalid preflight", http.StatusBadRequest)
			return
		}
		if logger != nil {
			logger.Printf(
				"client_preflight accepted=%t reason=%s detail=%q",
				decision.Accepted,
				decision.Reason,
				decision.Detail,
			)
		}
		if audit != nil && !decision.Accepted {
			audit.Emit(request.Context(), AuditEvent{
				OccurredAt: now().UTC(),
				ServerID:   input.ServerID,
				PlayerName: "unknown",
				PlayerRef:  "unavailable",
				Action:     "PREFLIGHT_REJECTED",
				Reason:     decision.Reason,
				Detail:     decision.Detail,
				Mods:       append([]string(nil), decision.Mods...),
				Rules: auditRulesWithEvidence(
					decision.Reason,
					input.Violations,
					decision.Detail,
				),
			})
		}
		response.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(response).Encode(decision); err != nil &&
			logger != nil {
			logger.Printf("encode preflight response: %v", err)
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
			if logger != nil {
				logger.Printf(
					"report_rejected user=%s reason=%q",
					compactUserID(strings.TrimSpace(report.UserID)),
					err.Error(),
				)
			}
			if audit != nil {
				audit.Emit(request.Context(), AuditEvent{
					OccurredAt: now().UTC(),
					ServerID:   report.ServerID,
					PlayerName: "unknown",
					PlayerRef: auditPlayerReference(
						serverToken,
						report.UserID,
					),
					Action: "REPORT_REJECTED",
					Reason: err.Error(),
				})
			}
			http.Error(response, err.Error(), http.StatusBadRequest)
			return
		}
		decision, err := store.EvaluatePreflight(ClientPreflight{
			ServerID:          report.ServerID,
			ProtocolVersion:   report.ProtocolVersion,
			Mods:              report.Mods,
			Violations:        report.Violations,
			ViolationEvidence: report.ViolationEvidence,
		})
		if err != nil {
			if logger != nil {
				logger.Printf(
					"report policy evaluation failed user=%s reason=%q",
					compactUserID(report.UserID),
					err.Error(),
				)
			}
			http.Error(
				response,
				"report policy evaluation failed",
				http.StatusInternalServerError,
			)
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
		if audit != nil && len(report.Violations) != 0 {
			detail := integrityDetail(
				report.Violations,
				report.ViolationEvidence,
			)
			audit.Emit(request.Context(), AuditEvent{
				OccurredAt: now().UTC(),
				ServerID:   report.ServerID,
				PlayerName: "unknown",
				PlayerRef: auditPlayerReference(
					serverToken,
					report.UserID,
				),
				Action: "INTEGRITY_REPORT_ACCEPTED",
				Reason: "INTEGRITY_VIOLATION",
				Detail: detail,
				Rules: append(
					[]string(nil),
					report.Violations...,
				),
			})
		}
		response.Header().Set("Content-Type", "application/json")
		response.WriteHeader(http.StatusAccepted)
		if err := json.NewEncoder(response).Encode(decision); err != nil &&
			logger != nil {
			logger.Printf("encode report policy response: %v", err)
		}
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
				!validPlayerName(player.Name, 96) {
				http.Error(
					response,
					"invalid player descriptor",
					http.StatusBadRequest,
				)
				return
			}
		}
		decisions := store.Evaluate(input.ServerID, input.Players, now())
		for _, decision := range decisions {
			if decision.Action != ActionKick {
				continue
			}
			if logger != nil {
				logger.Printf(
					"kick_decision user=%s reason=%s mods=%s detail=%q",
					compactUserID(decision.UserID),
					decision.Reason,
					strings.Join(decision.Mods, ","),
					decision.Detail,
				)
			}
			if audit != nil {
				audit.Emit(request.Context(), AuditEvent{
					OccurredAt: now().UTC(),
					ServerID:   input.ServerID,
					PlayerName: safeAuditPlayerName(decision.Name),
					PlayerRef: auditPlayerReference(
						serverToken,
						decision.UserID,
					),
					Action: "KICK_DECISION",
					Reason: decision.Reason,
					Detail: decision.Detail,
					Mods:   append([]string(nil), decision.Mods...),
					Rules: auditRulesWithEvidence(
						decision.Reason,
						decision.Violations,
						decision.Detail,
					),
				})
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
