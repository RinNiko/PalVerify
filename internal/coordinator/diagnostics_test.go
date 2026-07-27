package coordinator

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

type recordingAuditSink struct {
	events []AuditEvent
}

type auditRoundTripFunc func(*http.Request) (*http.Response, error)

func (function auditRoundTripFunc) RoundTrip(
	request *http.Request,
) (*http.Response, error) {
	return function(request)
}

func (sink *recordingAuditSink) Emit(
	_ context.Context,
	event AuditEvent,
) {
	sink.events = append(sink.events, event)
}

// A missing report must explain, in the decision detail, that no report was
// ever received so operators can tell "client never reported" apart from
// "client reported but the report was rejected/stale".
func TestEvaluateMissingReportCarriesNoReportDetail(t *testing.T) {
	now := time.Date(2026, 7, 25, 9, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods:  map[string]AllowedMod{},
	})
	players := []OnlinePlayer{{
		UserID: "steam_00000000000000001",
		Name:   "TestPlayer",
	}}

	waiting := store.Evaluate("bnb", players, now)
	if waiting[0].Action != ActionWait ||
		!strings.Contains(waiting[0].Detail, "NO_VALID_REPORT") {
		t.Fatalf("grace wait must explain NO_VALID_REPORT, got %#v", waiting)
	}

	kicked := store.Evaluate("bnb", players, now.Add(20*time.Second))
	if kicked[0].Reason != "MISSING_PALVERIFY" ||
		!strings.Contains(kicked[0].Detail, "NO_VALID_REPORT") {
		t.Fatalf("missing kick must carry NO_VALID_REPORT detail, got %#v", kicked)
	}
}

// A report that arrived but is older than ReportMaxAge must be reported as
// stale (with age) instead of looking identical to a never-seen client.
func TestEvaluateStaleReportCarriesStaleDetail(t *testing.T) {
	now := time.Date(2026, 7, 25, 9, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:     20 * time.Second,
		ReportMaxAge:    15 * time.Second,
		ChallengeMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.6",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})

	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_00000000000000001",
		Sequence: 1,
		SentAt:   now,
		Mods: []ReportedMod{{
			ID:      "PalVerify",
			Version: "0.5.6",
			Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		}},
	}, now)
	if err != nil {
		t.Fatalf("accept report: %v", err)
	}

	stale := store.Evaluate("bnb", []OnlinePlayer{{
		UserID: "steam_00000000000000001",
		Name:   "TestPlayer",
	}}, now.Add(16*time.Second))
	if !strings.Contains(stale[0].Detail, "STALE_REPORT") ||
		!strings.Contains(stale[0].Detail, "age=") {
		t.Fatalf("stale report must expose age detail, got %#v", stale)
	}
}

// An unapproved-mod kick must record exactly which rule each rejected mod
// tripped: not whitelisted, version mismatch, digest mismatch, or missing.
func TestEvaluateUnapprovedModDetailNamesTheRule(t *testing.T) {
	now := time.Date(2026, 7, 25, 9, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.6",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})

	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_00000000000000001",
		Sequence: 1,
		SentAt:   now,
		Mods: []ReportedMod{
			// Correct id but stale version -> version_mismatch.
			{ID: "PalVerify", Version: "0.5.0", Digest: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
			// Unknown mod -> not_whitelisted.
			{ID: "InfiniteStamina", Version: "1.0", Digest: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
		},
	}, now)
	if err != nil {
		t.Fatalf("accept report: %v", err)
	}

	result := store.Evaluate("bnb", []OnlinePlayer{{
		UserID: "steam_00000000000000001",
		Name:   "TestPlayer",
	}}, now)
	if result[0].Reason != "UNAPPROVED_MOD" {
		t.Fatalf("expected unapproved kick, got %#v", result)
	}
	if !strings.Contains(result[0].Detail, "VERSION_MISMATCH") ||
		!strings.Contains(result[0].Detail, "NOT_WHITELISTED") {
		t.Fatalf("detail must name each mod rule, got %q", result[0].Detail)
	}
}

// An integrity kick must list the triggered rule codes in the detail so an
// operator sees which detection fired (e.g. INJECTED_MODULE_DETECTED).
func TestEvaluateIntegrityDetailListsViolationRules(t *testing.T) {
	now := time.Date(2026, 7, 25, 9, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.6",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})

	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_00000000000000001",
		Sequence: 1,
		SentAt:   now,
		Mods: []ReportedMod{{
			ID:      "PalVerify",
			Version: "0.5.6",
			Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		}},
		Violations: []string{"INJECTED_MODULE_DETECTED"},
	}, now)
	if err != nil {
		t.Fatalf("accept report: %v", err)
	}

	result := store.Evaluate("bnb", []OnlinePlayer{{
		UserID: "steam_00000000000000001",
		Name:   "TestPlayer",
	}}, now)
	if result[0].Reason != "INTEGRITY_VIOLATION" ||
		!strings.Contains(result[0].Detail, "INJECTED_MODULE_DETECTED") {
		t.Fatalf("integrity detail must list rules, got %#v", result)
	}
}

// The coordinator must log why a client challenge or report was rejected, and
// kick decisions must carry their diagnostic detail into the log line.
func TestHandlerLogsRejectionsAndKickDetail(t *testing.T) {
	now := time.Date(2026, 7, 25, 9, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:     20 * time.Second,
		ReportMaxAge:    15 * time.Second,
		ChallengeMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.6",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})
	var logs bytes.Buffer
	logger := log.New(&logs, "", 0)
	clock := now
	handler := NewHandler(store, "server-token", func() time.Time { return clock }, logger)

	// Challenge without an active session must be logged as rejected.
	challengeReq := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/challenge",
		strings.NewReader(`{"serverId":"bnb","userId":"steam_00000000000000001"}`),
	)
	challengeReq.Header.Set("Content-Type", "application/json")
	handler.ServeHTTP(httptest.NewRecorder(), challengeReq)
	if !strings.Contains(logs.String(), "challenge_rejected") {
		t.Fatalf("expected challenge_rejected log, got %q", logs.String())
	}

	// Activate the session and mint a real challenge.
	activate := httptest.NewRequest(
		http.MethodPost,
		"/v1/server/evaluate",
		strings.NewReader(`{"serverId":"bnb","sentAt":"2026-07-25T09:00:00Z","players":[{"userId":"steam_00000000000000001","name":"TestPlayer"}]}`),
	)
	activate.Header.Set("Authorization", "Bearer server-token")
	activate.Header.Set("Content-Type", "application/json")
	handler.ServeHTTP(httptest.NewRecorder(), activate)

	challengeReq = httptest.NewRequest(
		http.MethodPost,
		"/v1/client/challenge",
		strings.NewReader(`{"serverId":"bnb","userId":"steam_00000000000000001"}`),
	)
	challengeReq.Header.Set("Content-Type", "application/json")
	challengeRec := httptest.NewRecorder()
	handler.ServeHTTP(challengeRec, challengeReq)
	var challengeBody challengeResponse
	if err := json.NewDecoder(challengeRec.Body).Decode(&challengeBody); err != nil {
		t.Fatalf("decode challenge: %v", err)
	}

	// A report with the wrong challenge must be logged with the exact reason.
	logs.Reset()
	badReport := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/report",
		strings.NewReader(`{"serverId":"bnb","userId":"steam_00000000000000001","protocolVersion":"3","challenge":"deadbeef","sequence":1,"sentAt":"2026-07-25T09:00:00Z","mods":[],"violations":[]}`),
	)
	badReport.Header.Set("Content-Type", "application/json")
	handler.ServeHTTP(httptest.NewRecorder(), badReport)
	if !strings.Contains(logs.String(), "report_rejected") ||
		!strings.Contains(logs.String(), "challenge") {
		t.Fatalf("expected report_rejected with reason, got %q", logs.String())
	}

	// After the grace period with no valid report, the kick log must include
	// the diagnostic detail (NO_VALID_REPORT).
	logs.Reset()
	clock = now.Add(21 * time.Second)
	kickEval := httptest.NewRequest(
		http.MethodPost,
		"/v1/server/evaluate",
		strings.NewReader(`{"serverId":"bnb","sentAt":"2026-07-25T09:00:21Z","players":[{"userId":"steam_00000000000000001","name":"TestPlayer"}]}`),
	)
	kickEval.Header.Set("Authorization", "Bearer server-token")
	kickEval.Header.Set("Content-Type", "application/json")
	handler.ServeHTTP(httptest.NewRecorder(), kickEval)
	if !strings.Contains(logs.String(), "kick_decision") ||
		!strings.Contains(logs.String(), "detail=") ||
		!strings.Contains(logs.String(), "NO_VALID_REPORT") {
		t.Fatalf("kick log must carry detail, got %q", logs.String())
	}
	_ = challengeBody
}

func TestHandlerAuditsRejectedPreflightWithoutPrivateIdentifiers(t *testing.T) {
	now := time.Date(2026, 7, 26, 10, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "1.0.6",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})
	sink := &recordingAuditSink{}
	handler := NewHandlerWithAudit(
		store,
		"server-token",
		func() time.Time { return now },
		nil,
		sink,
	)

	request := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/preflight",
		strings.NewReader(`{
			"serverId":"bnb",
			"protocolVersion":"3",
			"mods":[{
				"id":"PalVerify",
				"version":"1.0.6",
				"digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			}],
			"violations":["PROCESS_WEMOD_RUNNING"]
		}`),
	)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("preflight status=%d", response.Code)
	}
	if len(sink.events) != 1 {
		t.Fatalf("expected one audit event, got %#v", sink.events)
	}
	event := sink.events[0]
	if event.Action != "PREFLIGHT_REJECTED" ||
		event.Reason != "INTEGRITY_VIOLATION" ||
		!strings.Contains(event.Detail, "PROCESS_WEMOD_RUNNING") {
		t.Fatalf("unexpected audit event: %#v", event)
	}
	if event.PlayerName != "unknown" || event.PlayerRef != "unavailable" {
		t.Fatalf("preflight identity must remain anonymous: %#v", event)
	}
}

func TestCoordinatorDiscordErrorDoesNotLeakWebhookSecret(t *testing.T) {
	const secret = "super-secret-webhook-token"
	sink := &DiscordAuditSink{
		client: &http.Client{
			Transport: auditRoundTripFunc(func(
				request *http.Request,
			) (*http.Response, error) {
				return nil, errors.New(
					"dial failed: " + request.URL.String(),
				)
			}),
		},
		webhookURL: "https://discord.com/api/webhooks/123/" + secret,
	}
	err := sink.send(context.Background(), AuditEvent{
		OccurredAt: time.Now(),
		ServerID:   "bnb",
		PlayerName: "unknown",
		PlayerRef:  "unavailable",
		Action:     "PREFLIGHT_REJECTED",
		Reason:     "INTEGRITY_VIOLATION",
	})
	if err == nil {
		t.Fatal("expected Discord transport failure")
	}
	if strings.Contains(err.Error(), secret) ||
		strings.Contains(err.Error(), "/api/webhooks/") {
		t.Fatalf("Discord error leaked webhook URL: %q", err)
	}
}

func TestDiscordAuditSinkDeduplicatesSameIncident(t *testing.T) {
	sink := &DiscordAuditSink{
		webhookURL: "https://discord.com/api/webhooks/123/redacted-token",
		queue:      make(chan AuditEvent, 4),
	}
	event := AuditEvent{
		OccurredAt: time.Date(2026, 7, 26, 11, 0, 0, 0, time.UTC),
		ServerID:   "bnb",
		PlayerName: "TestPlayer",
		PlayerRef:  "pv_test",
		Action:     "KICK_DECISION",
		Reason:     "INTEGRITY_VIOLATION",
		Detail:     "PROCESS_WEMOD_RUNNING",
	}
	sink.Emit(context.Background(), event)
	event.OccurredAt = event.OccurredAt.Add(time.Second)
	sink.Emit(context.Background(), event)

	if len(sink.queue) != 1 {
		t.Fatalf(
			"same incident should be queued once, queued=%d",
			len(sink.queue),
		)
	}
}
