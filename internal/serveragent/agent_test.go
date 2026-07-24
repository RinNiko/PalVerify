package serveragent

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

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
		func(response http.ResponseWriter, request *http.Request) {
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
