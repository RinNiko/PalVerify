package serveragent

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func TestSyncOnceEvaluatesAutoAdminSessionForMissingHeartbeat(t *testing.T) {
	logDirectory := t.TempDir()
	if err := os.WriteFile(
		filepath.Join(logDirectory, "27.07 09.50.00.log"),
		[]byte("[09:50:38][info] 'RayJacobs' (UserId=steam_00000000000000001, IP=203.0.113.10) has logged in with auto admin mode.\n"),
		0o600,
	); err != nil {
		t.Fatal(err)
	}

	var evaluated []onlinePlayer
	coordinator := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		var body evaluateRequest
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Fatalf("decode evaluation request: %v", err)
		}
		evaluated = body.Players
		_ = json.NewEncoder(response).Encode(evaluateResponse{
			Decisions: []decision{{
				UserID: "steam_00000000000000001",
				Name:   "RayJacobs",
				Action: "kick",
				Reason: "MISSING_PALVERIFY",
				Detail: "NO_VALID_REPORT",
			}},
		})
	}))
	defer coordinator.Close()

	kickCount := 0
	rest := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		switch {
		case strings.HasSuffix(request.URL.Path, "/v1/pdapi/players"):
			_ = json.NewEncoder(response).Encode(playersDocument{
				Players: []player{{
					Name:   "RayJacobs",
					Status: "Admin",
					UserID: "steam_00000000000000001",
					IP:     "203.0.113.10",
				}},
			})
		case strings.Contains(request.URL.Path, "/v1/pdapi/kick/"):
			kickCount++
			response.WriteHeader(http.StatusOK)
		default:
			response.WriteHeader(http.StatusNotFound)
		}
	}))
	defer rest.Close()

	config := Config{
		Endpoint:                coordinator.URL,
		ServerToken:             "server-token",
		ServerID:                "bnb",
		RestURL:                 rest.URL,
		RestToken:               "rest-token",
		IntervalSeconds:         5,
		DisciplineStatePath:     filepath.Join(t.TempDir(), "discipline.json"),
		PalDefenderLogDirectory: logDirectory,
	}
	kicks, err := SyncOnce(
		context.Background(),
		rest.Client(),
		config,
		time.Now(),
		log.New(io.Discard, "", 0),
	)
	if err != nil {
		t.Fatal(err)
	}
	if len(evaluated) != 1 ||
		evaluated[0].UserID != "steam_00000000000000001" {
		t.Fatalf("auto-admin session was omitted from heartbeat evaluation: %#v", evaluated)
	}
	if kicks != 1 || kickCount != 1 {
		t.Fatalf("missing heartbeat was not enforced: kicks=%d restCalls=%d", kicks, kickCount)
	}
}

func (function roundTripFunc) RoundTrip(
	request *http.Request,
) (*http.Response, error) {
	return function(request)
}

// formatKickReason must fold the coordinator diagnostic detail into the
// player-facing kick message when present, so a kicked player (and the log)
// sees why they were blocked, not just the bare reason code.
func TestFormatKickReasonIncludesDetail(t *testing.T) {
	reason := formatKickReason(decision{
		Reason: "MISSING_PALVERIFY",
		Detail: "NO_VALID_REPORT",
	})
	if !strings.Contains(reason, "MISSING_PALVERIFY") ||
		!strings.Contains(reason, "NO_VALID_REPORT") {
		t.Fatalf("kick reason must carry detail, got %q", reason)
	}
}

// SyncOnce must log every kick with a compact user id, the reason, the
// rejected mods, and the diagnostic detail, so an operator reading the game
// server console can debug MISSING_PALVERIFY / UNAPPROVED_MOD without shelling
// into the coordinator VPS.
func TestSyncOnceLogsEachKickWithDetail(t *testing.T) {
	var webhookBody string
	webhook := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		body, err := io.ReadAll(request.Body)
		if err != nil {
			t.Fatalf("read webhook: %v", err)
		}
		webhookBody = string(body)
		response.WriteHeader(http.StatusNoContent)
	}))
	defer webhook.Close()

	rest := httptest.NewServer(http.HandlerFunc(func(
		w http.ResponseWriter,
		r *http.Request,
	) {
		switch {
		case strings.HasSuffix(r.URL.Path, "/v1/pdapi/players"):
			_ = json.NewEncoder(w).Encode(playersDocument{
				Players: []player{{
					Name:   "TestPlayer",
					Status: "online",
					UserID: "steam_00000000000000001",
				}},
			})
		case strings.Contains(r.URL.Path, "/v1/pdapi/kick/"):
			w.WriteHeader(http.StatusOK)
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer rest.Close()

	coordinator := httptest.NewServer(http.HandlerFunc(func(
		w http.ResponseWriter,
		_ *http.Request,
	) {
		_ = json.NewEncoder(w).Encode(evaluateResponse{
			Decisions: []decision{{
				UserID: "steam_00000000000000001",
				Name:   "TestPlayer",
				Action: "kick",
				Reason: "MISSING_PALVERIFY",
				Detail: "NO_VALID_REPORT",
			}},
		})
	}))
	defer coordinator.Close()

	config := Config{
		Endpoint:          coordinator.URL,
		ServerToken:       "server-token",
		ServerID:          "bnb",
		RestURL:           rest.URL,
		RestToken:         "rest-token",
		IntervalSeconds:   5,
		DiscordWebhookURL: webhook.URL,
		DisciplineStatePath: filepath.Join(
			t.TempDir(),
			"discipline.json",
		),
	}

	var logs bytes.Buffer
	logger := log.New(&logs, "", 0)
	kicks, err := SyncOnce(
		context.Background(),
		rest.Client(),
		config,
		time.Now(),
		logger,
	)
	if err != nil {
		t.Fatalf("sync: %v", err)
	}
	if kicks != 1 {
		t.Fatalf("expected 1 kick, got %d", kicks)
	}
	output := logs.String()
	if !strings.Contains(output, "kicked") ||
		!strings.Contains(output, "MISSING_PALVERIFY") ||
		!strings.Contains(output, "NO_VALID_REPORT") {
		t.Fatalf("kick log must include reason and detail, got %q", output)
	}
	// The raw full user id must not be logged verbatim; only a compact form.
	if strings.Contains(output, "steam_00000000000000001") {
		t.Fatalf("full user id must not appear in logs, got %q", output)
	}
	for _, expected := range []string{
		"TestPlayer",
		"MISSING_PALVERIFY",
		"NO_VALID_REPORT",
		"player_ref",
	} {
		if !strings.Contains(webhookBody, expected) {
			t.Fatalf("webhook missing %q: %s", expected, webhookBody)
		}
	}
	if strings.Contains(webhookBody, "steam_00000000000000001") {
		t.Fatalf("webhook leaked full user id: %s", webhookBody)
	}
}

func TestSyncOncePostsSafeAuditToAdminEndpoint(t *testing.T) {
	var auditBody string
	var auditAuthorization string
	coordinator := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		switch request.URL.Path {
		case "/evaluate":
			_ = json.NewEncoder(response).Encode(evaluateResponse{
				Decisions: []decision{{
					UserID: "steam_00000000000000001",
					Name:   "TestPlayer",
					Action: "kick",
					Reason: "MISSING_PALVERIFY",
					Detail: "NO_VALID_REPORT",
				}},
			})
		case "/audit":
			body, err := io.ReadAll(request.Body)
			if err != nil {
				t.Fatalf("read admin audit: %v", err)
			}
			auditBody = string(body)
			auditAuthorization = request.Header.Get("Authorization")
			response.WriteHeader(http.StatusAccepted)
		default:
			response.WriteHeader(http.StatusNotFound)
		}
	}))
	defer coordinator.Close()

	rest := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		switch {
		case strings.HasSuffix(request.URL.Path, "/v1/pdapi/players"):
			_ = json.NewEncoder(response).Encode(playersDocument{
				Players: []player{{
					Name:   "TestPlayer",
					Status: "online",
					UserID: "steam_00000000000000001",
					IP:     "203.0.113.10",
				}},
			})
		case strings.Contains(request.URL.Path, "/v1/pdapi/kick/"):
			response.WriteHeader(http.StatusOK)
		default:
			response.WriteHeader(http.StatusNotFound)
		}
	}))
	defer rest.Close()

	config := Config{
		Endpoint:            coordinator.URL + "/evaluate",
		ServerToken:         "server-token",
		ServerID:            "bnb",
		RestURL:             rest.URL,
		RestToken:           "rest-token",
		IntervalSeconds:     5,
		DisciplineStatePath: filepath.Join(t.TempDir(), "discipline.json"),
	}
	if _, err := SyncOnce(
		context.Background(),
		&http.Client{Timeout: time.Second},
		config,
		time.Now(),
		nil,
	); err != nil {
		t.Fatalf("sync: %v", err)
	}

	if auditAuthorization != "Bearer server-token" {
		t.Fatalf("admin audit missing bearer token: %q", auditAuthorization)
	}
	for _, expected := range []string{
		"TestPlayer",
		"MISSING_PALVERIFY",
		"NO_VALID_REPORT",
		"pv_",
		"steam_00000000000000001",
		"203.0.113.10",
	} {
		if !strings.Contains(auditBody, expected) {
			t.Fatalf("admin audit missing %q: %s", expected, auditBody)
		}
	}
	if strings.Contains(auditBody, "0001-01-01") {
		t.Fatalf("admin audit included a zero timestamp: %s", auditBody)
	}
}

func TestSyncOnceKicksOnlyCoordinatorKickDecisions(t *testing.T) {
	var kicked string
	var kickReason string
	palDefender := httptest.NewServer(http.HandlerFunc(
		func(response http.ResponseWriter, request *http.Request) {
			switch {
			case request.Method == http.MethodGet &&
				request.URL.Path == "/v1/pdapi/players":
				_ = json.NewEncoder(response).Encode(map[string]any{
					"Players": []map[string]any{
						{
							"Name":   "Bao",
							"Status": "Online",
							"UserId": "steam_76561198317031083",
						},
					},
				})
			case request.Method == http.MethodPost:
				kicked = request.URL.Path
				var body map[string]string
				_ = json.NewDecoder(request.Body).Decode(&body)
				kickReason = body["Reason"]
				response.WriteHeader(http.StatusOK)
			default:
				http.NotFound(response, request)
			}
		},
	))
	defer palDefender.Close()

	coordinator := httptest.NewServer(http.HandlerFunc(
		func(response http.ResponseWriter, _ *http.Request) {
			_ = json.NewEncoder(response).Encode(map[string]any{
				"decisions": []map[string]any{
					{
						"userId": "steam_76561198317031083",
						"name":   "Bao",
						"action": "kick",
						"reason": "UNAPPROVED_MOD",
						"mods":   []string{"InfiniteStamina"},
					},
				},
			})
		},
	))
	defer coordinator.Close()

	config := Config{
		Endpoint:        coordinator.URL,
		ServerToken:     "server-secret",
		ServerID:        "bnb",
		RestURL:         palDefender.URL,
		RestToken:       "rest-secret",
		IntervalSeconds: 5,
	}

	count, err := SyncOnce(
		context.Background(),
		&http.Client{Timeout: time.Second},
		config,
		time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC),
		nil,
	)
	if err != nil {
		t.Fatalf("sync once: %v", err)
	}
	if count != 1 {
		t.Fatalf("expected one kick, got %d", count)
	}
	if kicked != "/v1/pdapi/kick/steam_76561198317031083" {
		t.Fatalf("unexpected kick endpoint: %q", kicked)
	}
	if kickReason != "PalVerify: UNAPPROVED_MOD (InfiniteStamina)" {
		t.Fatalf("unexpected kick reason: %q", kickReason)
	}
}

func TestSyncOnceUsesAnonymousReferenceWhenPlayerNameIsMissing(t *testing.T) {
	const userID = "steam_76561198317031083"
	palDefender := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		if request.URL.Path != "/v1/pdapi/players" {
			http.NotFound(response, request)
			return
		}
		_ = json.NewEncoder(response).Encode(playersDocument{
			Players: []player{{
				Name:   "",
				Status: "Online",
				UserID: userID,
			}},
		})
	}))
	defer palDefender.Close()

	var evaluated evaluateRequest
	coordinator := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		if err := json.NewDecoder(request.Body).Decode(&evaluated); err != nil {
			t.Fatalf("decode evaluation: %v", err)
		}
		_ = json.NewEncoder(response).Encode(evaluateResponse{})
	}))
	defer coordinator.Close()

	config := Config{
		Endpoint:        coordinator.URL,
		ServerToken:     "server-secret",
		ServerID:        "bnb",
		RestURL:         palDefender.URL,
		RestToken:       "rest-secret",
		IntervalSeconds: 1,
	}
	if _, err := SyncOnce(
		context.Background(),
		&http.Client{Timeout: time.Second},
		config,
		time.Now(),
		nil,
	); err != nil {
		t.Fatalf("sync: %v", err)
	}
	if len(evaluated.Players) != 1 ||
		evaluated.Players[0].UserID != userID ||
		evaluated.Players[0].Name != "unknown" {
		t.Fatalf("missing-name player was not safely represented: %#v", evaluated)
	}
}

func TestIntegrityDisciplineEscalatesFrom24HoursToPermanent(t *testing.T) {
	const userID = "steam_76561198317031083"
	const playerName = "Bao"

	var now = time.Date(2026, 7, 26, 8, 0, 0, 0, time.UTC)
	var online = true
	var integrity = true
	var banCalls int
	var unbanCalls int
	var kickCalls int
	var webhookBodies []string

	palDefender := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		switch {
		case request.Method == http.MethodGet &&
			request.URL.Path == "/v1/pdapi/players":
			players := []player{}
			if online {
				players = append(players, player{
					Name:   playerName,
					Status: "Online",
					UserID: userID,
				})
			}
			_ = json.NewEncoder(response).Encode(playersDocument{
				Players: players,
			})
		case request.Method == http.MethodPost &&
			request.URL.Path == "/v1/pdapi/ban/"+userID:
			banCalls++
			var body map[string]any
			if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
				t.Fatalf("decode ban body: %v", err)
			}
			if body["IP"] == true {
				t.Fatal("integrity discipline must not IP-ban the player")
			}
			response.WriteHeader(http.StatusOK)
		case request.Method == http.MethodPost &&
			request.URL.Path == "/v1/pdapi/unban/"+userID:
			unbanCalls++
			response.WriteHeader(http.StatusOK)
		case strings.Contains(request.URL.Path, "/v1/pdapi/kick/"):
			kickCalls++
			response.WriteHeader(http.StatusOK)
		default:
			http.NotFound(response, request)
		}
	}))
	defer palDefender.Close()

	coordinator := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		_ *http.Request,
	) {
		decisions := []decision{}
		if online && integrity {
			decisions = append(decisions, decision{
				UserID: userID,
				Name:   playerName,
				Action: "kick",
				Reason: "INTEGRITY_VIOLATION",
				Detail: "PROCESS_WEMOD_RUNNING,INJECTED_MODULE_DETECTED",
			})
		}
		_ = json.NewEncoder(response).Encode(evaluateResponse{
			Decisions: decisions,
		})
	}))
	defer coordinator.Close()

	webhook := httptest.NewServer(http.HandlerFunc(func(
		response http.ResponseWriter,
		request *http.Request,
	) {
		body, err := io.ReadAll(request.Body)
		if err != nil {
			t.Fatalf("read webhook body: %v", err)
		}
		webhookBodies = append(webhookBodies, string(body))
		response.WriteHeader(http.StatusNoContent)
	}))
	defer webhook.Close()

	config := Config{
		Endpoint:            coordinator.URL,
		ServerToken:         "server-secret",
		ServerID:            "bnb",
		RestURL:             palDefender.URL,
		RestToken:           "rest-secret",
		IntervalSeconds:     1,
		DiscordWebhookURL:   webhook.URL,
		DisciplineStatePath: filepath.Join(t.TempDir(), "discipline.json"),
	}
	client := &http.Client{Timeout: time.Second}

	if _, err := SyncOnce(
		context.Background(),
		client,
		config,
		now,
		nil,
	); err != nil {
		t.Fatalf("first integrity sync: %v", err)
	}
	if banCalls != 1 || kickCalls != 0 {
		t.Fatalf(
			"first integrity incident must ban once, bans=%d kicks=%d",
			banCalls,
			kickCalls,
		)
	}
	if len(webhookBodies) != 1 {
		t.Fatalf("first incident must emit one webhook, got %d", len(webhookBodies))
	}
	firstWebhook := webhookBodies[0]
	for _, expected := range []string{
		playerName,
		"PROCESS_WEMOD_RUNNING",
		"INJECTED_MODULE_DETECTED",
		"24",
		"player_ref",
	} {
		if !strings.Contains(firstWebhook, expected) {
			t.Fatalf("first webhook missing %q: %s", expected, firstWebhook)
		}
	}
	if strings.Contains(firstWebhook, userID) ||
		strings.Contains(strings.ToLower(firstWebhook), "\"ip\"") {
		t.Fatalf("webhook leaked a private identifier: %s", firstWebhook)
	}

	// Repeated coordinator polls for the same still-active incident must not
	// turn the first 24-hour ban into a permanent ban.
	now = now.Add(time.Minute)
	if _, err := SyncOnce(
		context.Background(),
		client,
		config,
		now,
		nil,
	); err != nil {
		t.Fatalf("duplicate integrity sync: %v", err)
	}
	if banCalls != 1 || len(webhookBodies) != 1 {
		t.Fatalf(
			"duplicate poll escalated or spammed: bans=%d webhooks=%d",
			banCalls,
			len(webhookBodies),
		)
	}

	// Once 24 hours have elapsed the agent must unban, even with no player
	// online, while retaining the prior-offense counter.
	online = false
	integrity = false
	now = now.Add(24*time.Hour + time.Second)
	if _, err := SyncOnce(
		context.Background(),
		client,
		config,
		now,
		nil,
	); err != nil {
		t.Fatalf("temporary unban sync: %v", err)
	}
	if unbanCalls != 1 {
		t.Fatalf("temporary ban was not removed, unban calls=%d", unbanCalls)
	}

	// A later, distinct integrity incident is the second offense and must
	// become permanent.
	online = true
	integrity = true
	now = now.Add(time.Minute)
	if _, err := SyncOnce(
		context.Background(),
		client,
		config,
		now,
		nil,
	); err != nil {
		t.Fatalf("repeat integrity sync: %v", err)
	}
	if banCalls != 2 {
		t.Fatalf("second incident did not re-ban, ban calls=%d", banCalls)
	}
	if len(webhookBodies) != 3 {
		t.Fatalf(
			"expected first ban, unban, and permanent-ban webhooks; got %d",
			len(webhookBodies),
		)
	}
	if !strings.Contains(strings.ToLower(webhookBodies[2]), "permanent") {
		t.Fatalf("permanent ban webhook missing escalation: %s", webhookBodies[2])
	}
}

func TestConfigRequiresHTTPSCoordinatorAndLoopbackPalDefender(t *testing.T) {
	config := Config{
		Endpoint:        "http://public.example.test/v1/server/evaluate",
		ServerToken:     "server-secret",
		ServerID:        "bnb",
		RestURL:         "http://203.0.113.1:17993",
		RestToken:       "rest-secret",
		IntervalSeconds: 5,
	}
	if err := config.Validate(); err == nil {
		t.Fatal("unsafe coordinator and PalDefender URLs accepted")
	}

	config.Endpoint = "https://ingest.example.test/v1/server/evaluate"
	config.RestURL = "http://127.0.0.1:17993"
	if err := config.Validate(); err != nil {
		t.Fatalf("safe config rejected: %v", err)
	}

	config.DiscordWebhookURL =
		"https://example.test/api/webhooks/not-discord"
	if err := config.Validate(); err == nil {
		t.Fatal("non-Discord webhook endpoint accepted")
	}
	config.DiscordWebhookURL =
		"https://discord.com/api/webhooks/123/redacted-token"
	if err := config.Validate(); err != nil {
		t.Fatalf("Discord webhook endpoint rejected: %v", err)
	}
}

func TestProcessAdminModerationActionExecutesPalDefenderAndAcknowledges(t *testing.T) {
	t.Parallel()

	var punishmentPath string
	var acknowledgement struct {
		ActionID string `json:"actionId"`
		Status   string `json:"status"`
		Message  string `json:"message"`
	}
	palDefender := httptest.NewServer(http.HandlerFunc(
		func(response http.ResponseWriter, request *http.Request) {
			punishmentPath = request.URL.Path
			if request.Header.Get("Authorization") != "Bearer rest-secret" {
				t.Fatal("PalDefender token missing")
			}
			response.WriteHeader(http.StatusOK)
		},
	))
	defer palDefender.Close()

	coordinator := httptest.NewServer(http.HandlerFunc(
		func(response http.ResponseWriter, request *http.Request) {
			switch request.Method {
			case http.MethodGet:
				if request.URL.Query().Get("serverId") != "bnb" {
					t.Fatalf(
						"moderation poll missing serverId: %q",
						request.URL.RawQuery,
					)
				}
				_ = json.NewEncoder(response).Encode(map[string]any{
					"action": map[string]any{
						"actionId":   "action_0123456789abcdef0123456789abcdef",
						"action":     "ban",
						"userId":     "steam_76561198317031083",
						"playerName": "RayJacobs",
						"playerRef":  "pv_e8471ff4d2ef",
						"reason":     "Admin: confirmed integrity violation",
					},
				})
			case http.MethodPost:
				if err := json.NewDecoder(request.Body).Decode(
					&acknowledgement,
				); err != nil {
					t.Fatal(err)
				}
				response.WriteHeader(http.StatusAccepted)
			default:
				t.Fatalf("unexpected method %s", request.Method)
			}
		},
	))
	defer coordinator.Close()

	config := Config{
		Endpoint:    coordinator.URL + "/api/palverify/v1/server/evaluate",
		ServerToken: "server-secret",
		ServerID:    "bnb",
		RestURL:     palDefender.URL,
		RestToken:   "rest-secret",
	}
	processed, err := processAdminModerationAction(
		context.Background(),
		coordinator.Client(),
		config,
	)
	if err != nil {
		t.Fatal(err)
	}
	if !processed {
		t.Fatal("expected queued moderation action to be processed")
	}
	if punishmentPath != "/v1/pdapi/ban/steam_76561198317031083" {
		t.Fatalf("unexpected punishment endpoint %q", punishmentPath)
	}
	if acknowledgement.ActionID !=
		"action_0123456789abcdef0123456789abcdef" ||
		acknowledgement.Status != "completed" {
		t.Fatalf("unexpected acknowledgement: %#v", acknowledgement)
	}
}

func TestDiscordTransportErrorDoesNotLeakWebhookSecret(t *testing.T) {
	const secret = "super-secret-webhook-token"
	client := &http.Client{
		Transport: roundTripFunc(func(
			request *http.Request,
		) (*http.Response, error) {
			return nil, errors.New("dial failed: " + request.URL.String())
		}),
	}
	err := sendDiscordAudit(
		context.Background(),
		client,
		"https://discord.com/api/webhooks/123/"+secret,
		discordAuditEvent{
			OccurredAt: time.Now(),
			ServerID:   "bnb",
			PlayerName: "TestPlayer",
			PlayerRef:  "pv_test",
			Action:     "KICK",
			Reason:     "MISSING_PALVERIFY",
		},
	)
	if err == nil {
		t.Fatal("expected Discord transport failure")
	}
	if strings.Contains(err.Error(), secret) ||
		strings.Contains(err.Error(), "/api/webhooks/") {
		t.Fatalf("Discord error leaked webhook URL: %q", err)
	}
}

func TestTransientStartupErrorRecognizesPalDefenderReadiness(t *testing.T) {
	tests := []struct {
		err       error
		transient bool
	}{
		{
			err: errors.New(
				"PalDefender players returned HTTP 403",
			),
			transient: true,
		},
		{
			err: errors.New(
				"read players: dial tcp 127.0.0.1:17993: connection refused",
			),
			transient: true,
		},
		{
			err:       errors.New("coordinator returned HTTP 400"),
			transient: false,
		},
	}

	for _, test := range tests {
		if got := IsTransientStartupError(test.err); got != test.transient {
			t.Fatalf(
				"IsTransientStartupError(%q) = %v, want %v",
				test.err,
				got,
				test.transient,
			)
		}
	}
}
