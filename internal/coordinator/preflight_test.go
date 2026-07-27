package coordinator

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

type preflightTestResponse struct {
	Accepted bool     `json:"accepted"`
	Reason   string   `json:"reason"`
	Detail   string   `json:"detail"`
	Mods     []string `json:"mods"`
}

func TestClientPreflightAcceptsApprovedPackageWithoutActiveSession(t *testing.T) {
	handler := preflightTestHandler()
	request := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/preflight",
		strings.NewReader(`{
			"serverId":"bnb",
			"protocolVersion":"3",
			"mods":[{
				"id":"PalVerify",
				"version":"1.0.1",
				"digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			}],
			"violations":[]
		}`),
	)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("preflight status = %d, want 200; body=%q", response.Code, response.Body.String())
	}
	var result preflightTestResponse
	if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
		t.Fatalf("decode preflight response: %v", err)
	}
	if !result.Accepted || result.Reason != "VERIFIED" {
		t.Fatalf("approved package must pass preflight, got %#v", result)
	}
}

func TestClientPreflightReturnsSafeDigestMismatchDetail(t *testing.T) {
	handler := preflightTestHandler()
	request := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/preflight",
		strings.NewReader(`{
			"serverId":"bnb",
			"protocolVersion":"3",
			"mods":[{
				"id":"PalVerify",
				"version":"1.0.1",
				"digest":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
			}],
			"violations":[]
		}`),
	)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	var result preflightTestResponse
	if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
		t.Fatalf("decode preflight response: %v", err)
	}
	if response.Code != http.StatusOK ||
		result.Accepted ||
		result.Reason != "UNAPPROVED_MOD" ||
		result.Detail != "PalVerify:DIGEST_MISMATCH" {
		t.Fatalf("digest mismatch must be explicit and safe, got status=%d result=%#v", response.Code, result)
	}
}

func TestClientPreflightReturnsIntegrityRuleImmediately(t *testing.T) {
	handler := preflightTestHandler()
	request := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/preflight",
		strings.NewReader(`{
			"serverId":"bnb",
			"protocolVersion":"3",
			"mods":[{
				"id":"PalVerify",
				"version":"1.0.1",
				"digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			}],
			"violations":["CHEAT_ENGINE_RUNNING"]
		}`),
	)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	var result preflightTestResponse
	if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
		t.Fatalf("decode preflight response: %v", err)
	}
	if response.Code != http.StatusOK ||
		result.Accepted ||
		result.Reason != "INTEGRITY_VIOLATION" ||
		result.Detail != "CHEAT_ENGINE_RUNNING" {
		t.Fatalf("integrity rule must be returned immediately, got status=%d result=%#v", response.Code, result)
	}
}

func TestClientPreflightRejectsMalformedDescriptor(t *testing.T) {
	handler := preflightTestHandler()
	request := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/preflight",
		strings.NewReader(`{
			"serverId":"bnb",
			"protocolVersion":"3",
			"mods":[{"id":"PalVerify","version":"1.0.1","digest":"not-a-digest"}],
			"violations":[]
		}`),
	)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusBadRequest {
		t.Fatalf("malformed descriptor status = %d, want 400", response.Code)
	}
}

func preflightTestHandler() http.Handler {
	return NewHandler(
		NewStore(Config{
			GracePeriod:     20 * time.Second,
			ReportMaxAge:    15 * time.Second,
			ChallengeMaxAge: 15 * time.Second,
			AllowedMods: map[string]AllowedMod{
				"PalVerify": {
					Version: "1.0.1",
					Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
				},
			},
		}),
		"server-token",
		time.Now,
		nil,
	)
}
