package coordinator

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func acceptReportForActivePlayer(
	t *testing.T,
	store *Store,
	report Report,
	now time.Time,
) error {
	t.Helper()
	store.Evaluate("bnb", []OnlinePlayer{{
		UserID: report.UserID,
		Name:   "Bao",
	}}, now)
	challenge, err := store.IssueChallenge("bnb", report.UserID, now)
	if err != nil {
		t.Fatalf("issue challenge: %v", err)
	}
	report.ServerID = "bnb"
	report.ProtocolVersion = "3"
	report.Challenge = challenge
	return store.AcceptReport(report, now)
}

func TestEvaluateAllowsFreshWhitelistedPalVerifyReport(t *testing.T) {
	now := time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.3.0",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})

	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_76561198317031083",
		Sequence: 1,
		SentAt:   now.Add(-time.Second),
		Mods: []ReportedMod{{
			ID:      "PalVerify",
			Version: "0.3.0",
			Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		}},
	}, now)
	if err != nil {
		t.Fatalf("accept report: %v", err)
	}

	result := store.Evaluate("bnb", []OnlinePlayer{{
		UserID: "steam_76561198317031083",
		Name:   "Bao",
	}}, now)

	if len(result) != 1 || result[0].Action != ActionAllow {
		t.Fatalf("expected allow, got %#v", result)
	}
}

func TestEvaluateUsesCoordinatorReceiptTimeInsteadOfClientClock(t *testing.T) {
	now := time.Date(2026, 7, 29, 8, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods:  map[string]AllowedMod{},
	})

	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_76561198317031083",
		Sequence: 1,
		// Legacy clients may send a wall-clock timestamp that is minutes off.
		SentAt: now.Add(-481 * time.Second),
	}, now)
	if err != nil {
		t.Fatalf("accept clock-skewed report: %v", err)
	}

	result := store.Evaluate("bnb", []OnlinePlayer{{
		UserID: "steam_76561198317031083",
		Name:   "Bao",
	}}, now)
	if len(result) != 1 || result[0].Action != ActionAllow {
		t.Fatalf("freshly received report must ignore client clock, got %#v", result)
	}
}

func TestAcceptReportDoesNotRequireClientTimestamp(t *testing.T) {
	now := time.Date(2026, 7, 29, 8, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods:  map[string]AllowedMod{},
	})

	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_76561198317031083",
		Sequence: 1,
	}, now)
	if err != nil {
		t.Fatalf("accept report without client timestamp: %v", err)
	}
}

func TestEvaluateKicksUnknownModAndLogsOnlyCompactDescriptors(t *testing.T) {
	now := time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.3.0",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})

	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_76561198317031083",
		Sequence: 1,
		SentAt:   now,
		Mods: []ReportedMod{
			{ID: "PalVerify", Version: "0.3.0", Digest: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
			{ID: "InfiniteStamina", Version: "1.0", Digest: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
		},
	}, now)
	if err != nil {
		t.Fatalf("accept report: %v", err)
	}

	result := store.Evaluate("bnb", []OnlinePlayer{{
		UserID: "steam_76561198317031083",
		Name:   "Bao",
	}}, now)

	if len(result) != 1 || result[0].Action != ActionKick {
		t.Fatalf("expected kick, got %#v", result)
	}
	if result[0].Reason != "UNAPPROVED_MOD" {
		t.Fatalf("unexpected reason: %q", result[0].Reason)
	}
	if len(result[0].Mods) != 1 || result[0].Mods[0] != "InfiniteStamina" {
		t.Fatalf("expected compact rejected mod id, got %#v", result[0].Mods)
	}
}

func TestApprovedDuplicatePackageCopiesAreAllowed(t *testing.T) {
	const digestA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	const digestB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	const digestC = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
	allowed := map[string]AllowedMod{
		"UE4SSExperimentalPW": {
			Version: "experimental-palworld-6",
			Digest:  digestA,
			CompatiblePackages: []AllowedPackage{{
				Version: "experimental-palworld-6",
				Digest:  digestB,
			}},
		},
	}
	approvedCopies := []ReportedMod{
		{
			ID:      "UE4SSExperimentalPW",
			Version: "experimental-palworld-6",
			Digest:  digestA,
		},
		{
			ID:      "UE4SSExperimentalPW",
			Version: "experimental-palworld-6",
			Digest:  digestB,
		},
	}

	rejected, detail := rejectedModIDs(approvedCopies, allowed)
	if len(rejected) != 0 || detail != "" {
		t.Fatalf("approved copies must pass, got mods=%#v detail=%q", rejected, detail)
	}

	approvedCopies[1].Digest = digestC
	rejected, detail = rejectedModIDs(approvedCopies, allowed)
	if len(rejected) != 1 ||
		rejected[0] != "UE4SSExperimentalPW" ||
		detail != "UE4SSExperimentalPW:DIGEST_MISMATCH" {
		t.Fatalf("unknown copy must be rejected, got mods=%#v detail=%q", rejected, detail)
	}
}

func TestEvaluateKicksDetectedCheatRule(t *testing.T) {
	now := time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.3.0",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})

	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_76561198317031083",
		Sequence: 1,
		SentAt:   now,
		Mods: []ReportedMod{{
			ID:      "PalVerify",
			Version: "0.3.0",
			Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		}},
		Violations: []string{
			"CHEAT_ENGINE_RUNNING",
			"INJECTED_MODULE_DETECTED",
		},
		ViolationEvidence: []IntegrityEvidence{{
			Rule:            "INJECTED_MODULE_DETECTED",
			Source:          "module",
			FileName:        "trainerlib_x64.dll",
			SHA256:          "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			SignerName:      "WeMod LLC",
			FileDescription: "TrainerLib Plugin",
			CompanyName:     "Wand Technologies",
			MatchReason:     "WEMOD_MODULE_SIGNATURE",
			SignatureValid:  true,
		}},
	}, now)
	if err != nil {
		t.Fatalf("accept report: %v", err)
	}

	result := store.Evaluate("bnb", []OnlinePlayer{{
		UserID: "steam_76561198317031083",
		Name:   "Bao",
	}}, now)

	if len(result) != 1 || result[0].Reason != "INTEGRITY_VIOLATION" {
		t.Fatalf("expected integrity kick, got %#v", result)
	}
	if !strings.Contains(result[0].Detail, "file=trainerlib_x64.dll") ||
		!strings.Contains(result[0].Detail, "sha256=bbbbbbbb") ||
		!strings.Contains(result[0].Detail, "signer=WeMod LLC") ||
		!strings.Contains(result[0].Detail, "description=TrainerLib Plugin") ||
		!strings.Contains(result[0].Detail, "company=Wand Technologies") ||
		!strings.Contains(result[0].Detail, "match=WEMOD_MODULE_SIGNATURE") {
		t.Fatalf("missing safe module evidence: %q", result[0].Detail)
	}
	if strings.Contains(result[0].Detail, `C:\`) {
		t.Fatalf("integrity detail leaked a full path: %q", result[0].Detail)
	}
}

func TestAcceptReportRejectsModuleEvidenceWithPath(t *testing.T) {
	now := time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
	})
	err := acceptReportForActivePlayer(t, store, Report{
		UserID:   "steam_76561198317031083",
		Sequence: 1,
		SentAt:   now,
		Violations: []string{
			"INJECTED_MODULE_DETECTED",
		},
		ViolationEvidence: []IntegrityEvidence{{
			Rule:        "INJECTED_MODULE_DETECTED",
			Source:      "module",
			FileName:    `C:\private\trainer.dll`,
			SHA256:      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			MatchReason: "UNSIGNED_EXTERNAL_MODULE",
		}},
	}, now)
	if err == nil || !strings.Contains(err.Error(), "evidence") {
		t.Fatalf("expected unsafe evidence rejection, got %v", err)
	}
}

func TestEvaluateWaitsThenKicksSteamWithoutReport(t *testing.T) {
	now := time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods:  map[string]AllowedMod{},
	})
	players := []OnlinePlayer{{
		UserID: "steam_76561198317031083",
		Name:   "Bao",
	}}

	first := store.Evaluate("bnb", players, now)
	if first[0].Action != ActionWait {
		t.Fatalf("expected grace wait, got %#v", first)
	}

	expired := store.Evaluate("bnb", players, now.Add(20*time.Second))
	if expired[0].Action != ActionKick ||
		expired[0].Reason != "MISSING_PALVERIFY" {
		t.Fatalf("expected missing-client kick, got %#v", expired)
	}
}

func TestEvaluateDoesNotKickUnresolvedGDKPlatform(t *testing.T) {
	now := time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods:  map[string]AllowedMod{},
	})

	result := store.Evaluate("bnb", []OnlinePlayer{{
		UserID: "gdk_2533274812345678",
		Name:   "ConsoleOrGamePass",
	}}, now)

	if len(result) != 1 || result[0].Action != ActionObserve {
		t.Fatalf("unresolved gdk must remain observation-only, got %#v", result)
	}
}

func TestAcceptReportRejectsReplayAndOversizedInventory(t *testing.T) {
	now := time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods:  map[string]AllowedMod{},
	})
	report := Report{
		UserID:          "steam_76561198317031083",
		ProtocolVersion: "3",
		Sequence:        2,
		SentAt:          now,
	}
	store.Evaluate("bnb", []OnlinePlayer{{
		UserID: report.UserID,
		Name:   "Bao",
	}}, now)
	challenge, err := store.IssueChallenge("bnb", report.UserID, now)
	if err != nil {
		t.Fatalf("issue challenge: %v", err)
	}
	report.ServerID = "bnb"
	report.Challenge = challenge
	if err := store.AcceptReport(report, now); err != nil {
		t.Fatalf("first report rejected: %v", err)
	}
	if err := store.AcceptReport(report, now); err == nil {
		t.Fatal("replayed sequence accepted")
	}

	report.UserID = "steam_76561198000000000"
	report.Mods = make([]ReportedMod, 65)
	store.Evaluate("bnb", []OnlinePlayer{{
		UserID: report.UserID,
		Name:   "Other",
	}}, now)
	challenge, err = store.IssueChallenge("bnb", report.UserID, now)
	if err != nil {
		t.Fatalf("issue second challenge: %v", err)
	}
	report.Challenge = challenge
	if err := store.AcceptReport(report, now); err == nil {
		t.Fatal("oversized mod inventory accepted")
	}
}

func TestActiveSessionChallengeIsRequiredAndSingleUse(t *testing.T) {
	now := time.Date(2026, 7, 24, 9, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:     20 * time.Second,
		ReportMaxAge:    15 * time.Second,
		ChallengeMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.0",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})
	player := OnlinePlayer{
		UserID: "steam_76561198317031083",
		Name:   "Bao",
	}

	if _, err := store.IssueChallenge("bnb", player.UserID, now); err == nil {
		t.Fatal("offline player received a challenge")
	}

	store.Evaluate("bnb", []OnlinePlayer{player}, now)
	challenge, err := store.IssueChallenge("bnb", player.UserID, now)
	if err != nil || challenge == "" {
		t.Fatalf("active player challenge: %q, %v", challenge, err)
	}

	report := Report{
		ServerID:        "bnb",
		UserID:          player.UserID,
		ProtocolVersion: "3",
		Challenge:       challenge,
		Sequence:        1,
		SentAt:          now,
		Mods: []ReportedMod{{
			ID:      "PalVerify",
			Version: "0.5.0",
			Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		}},
	}
	if err := store.AcceptReport(report, now); err != nil {
		t.Fatalf("active challenge report rejected: %v", err)
	}
	if err := store.AcceptReport(report, now); err == nil {
		t.Fatal("single-use challenge was replayed")
	}

	decisions := store.Evaluate("bnb", []OnlinePlayer{player}, now)
	if len(decisions) != 1 || decisions[0].Action != ActionAllow {
		t.Fatalf("challenge-bound report did not allow player: %#v", decisions)
	}
}

func TestHTTPHandlerUsesSessionChallengeAndProtectsServerCredential(t *testing.T) {
	now := time.Date(2026, 7, 24, 7, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:     20 * time.Second,
		ReportMaxAge:    15 * time.Second,
		ChallengeMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "0.5.0",
				Digest:  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			},
		},
	})
	handler := NewHandler(
		store,
		"server-token",
		func() time.Time { return now },
		nil,
	)

	evaluateRequest := httptest.NewRequest(
		http.MethodPost,
		"/v1/server/evaluate",
		strings.NewReader(`{
			"serverId":"bnb",
			"sentAt":"2026-07-24T07:00:00Z",
			"players":[
				{"userId":"steam_76561198317031083","name":"Bao"}
			]
		}`),
	)
	evaluateRequest.Header.Set("Authorization", "Bearer server-token")
	evaluateRequest.Header.Set("Content-Type", "application/json")
	evaluateResponse := httptest.NewRecorder()
	handler.ServeHTTP(evaluateResponse, evaluateRequest)
	if evaluateResponse.Code != http.StatusOK {
		t.Fatalf("evaluate status: %d", evaluateResponse.Code)
	}

	challengeRequest := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/challenge",
		strings.NewReader(`{
			"serverId":"bnb",
			"userId":"steam_76561198317031083"
		}`),
	)
	challengeRequest.Header.Set("Content-Type", "application/json")
	challengeResult := httptest.NewRecorder()
	handler.ServeHTTP(challengeResult, challengeRequest)
	if challengeResult.Code != http.StatusOK {
		t.Fatalf("challenge status: %d", challengeResult.Code)
	}
	var challengeBody challengeResponse
	if err := json.NewDecoder(challengeResult.Body).Decode(
		&challengeBody,
	); err != nil || challengeBody.Challenge == "" {
		t.Fatalf("decode challenge: %#v, %v", challengeBody, err)
	}

	reportBody := `{
		"serverId":"bnb",
		"userId":"steam_76561198317031083",
		"protocolVersion":"3",
		"challenge":"` + challengeBody.Challenge + `",
		"sequence":1,
		"sentAt":"2026-07-24T06:59:59Z",
		"mods":[
			{"id":"PalVerify","version":"0.5.0","digest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
		],
		"violations":[]
	}`
	reportRequest := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/report",
		strings.NewReader(reportBody),
	)
	reportRequest.Header.Set("Content-Type", "application/json")
	reportResponse := httptest.NewRecorder()
	handler.ServeHTTP(reportResponse, reportRequest)
	if reportResponse.Code != http.StatusAccepted {
		t.Fatalf("report status: %d", reportResponse.Code)
	}

	evaluateRequest = httptest.NewRequest(
		http.MethodPost,
		"/v1/server/evaluate",
		strings.NewReader(`{
			"serverId":"bnb",
			"sentAt":"2026-07-24T07:00:00Z",
			"players":[
				{"userId":"steam_76561198317031083","name":"Bao"}
			]
		}`),
	)
	evaluateRequest.Header.Set("Authorization", "Bearer server-token")
	evaluateRequest.Header.Set("Content-Type", "application/json")
	evaluateResponse = httptest.NewRecorder()
	handler.ServeHTTP(evaluateResponse, evaluateRequest)

	var body struct {
		Decisions []Decision `json:"decisions"`
	}
	if err := json.NewDecoder(evaluateResponse.Body).Decode(&body); err != nil {
		t.Fatalf("decode evaluate response: %v", err)
	}
	if len(body.Decisions) != 1 || body.Decisions[0].Action != ActionAllow {
		t.Fatalf("unexpected decisions: %#v", body.Decisions)
	}

	wrongCredential := httptest.NewRequest(
		http.MethodPost,
		"/v1/server/evaluate",
		strings.NewReader(`{"serverId":"bnb","players":[]}`),
	)
	wrongCredential.Header.Set("Authorization", "Bearer public-client")
	wrongCredential.Header.Set("Content-Type", "application/json")
	wrongResponse := httptest.NewRecorder()
	handler.ServeHTTP(wrongResponse, wrongCredential)
	if wrongResponse.Code != http.StatusUnauthorized {
		t.Fatalf("public client reached server API: %d", wrongResponse.Code)
	}
}

func TestHTTPReportReturnsPlayerFacingPolicyDecision(t *testing.T) {
	now := time.Date(2026, 7, 28, 9, 0, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:  20 * time.Second,
		ReportMaxAge: 15 * time.Second,
		AllowedMods: map[string]AllowedMod{
			"PalVerify": {
				Version: "1.0.13",
				Digest:  strings.Repeat("a", 64),
			},
		},
	})
	handler := NewHandler(
		store,
		"server-token",
		func() time.Time { return now },
		nil,
	)
	userID := "steam_76561198317031083"
	store.Evaluate("bnb", []OnlinePlayer{{
		UserID: userID,
		Name:   "Bao",
	}}, now)
	challenge, err := store.IssueChallenge("bnb", userID, now)
	if err != nil {
		t.Fatalf("issue challenge: %v", err)
	}
	body, err := json.Marshal(Report{
		ServerID:        "bnb",
		UserID:          userID,
		ProtocolVersion: "3",
		Challenge:       challenge,
		Sequence:        1,
		SentAt:          now,
		Mods: []ReportedMod{{
			ID:      "PalVerify",
			Version: "1.0.12",
			Digest:  strings.Repeat("b", 64),
		}},
	})
	if err != nil {
		t.Fatalf("encode report: %v", err)
	}
	request := httptest.NewRequest(
		http.MethodPost,
		"/v1/client/report",
		strings.NewReader(string(body)),
	)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusAccepted {
		t.Fatalf("report status: %d", response.Code)
	}

	var decision PreflightDecision
	if err := json.NewDecoder(response.Body).Decode(&decision); err != nil {
		t.Fatalf("decode report policy decision: %v", err)
	}
	if decision.Accepted ||
		decision.Reason != "UNAPPROVED_MOD" ||
		!strings.Contains(decision.Detail, "PalVerify:VERSION_MISMATCH") ||
		len(decision.Mods) != 1 ||
		decision.Mods[0] != "PalVerify" {
		t.Fatalf("unexpected report policy decision: %#v", decision)
	}
}

func TestHTTPHandlerAcceptsUnicodePlayerNamesForEvaluation(t *testing.T) {
	now := time.Date(2026, 7, 27, 10, 30, 0, 0, time.UTC)
	store := NewStore(Config{
		GracePeriod:     20 * time.Second,
		ReportMaxAge:    45 * time.Second,
		ChallengeMaxAge: 15 * time.Second,
		AllowedMods:     map[string]AllowedMod{},
	})
	handler := NewHandler(
		store,
		"server-token",
		func() time.Time { return now },
		nil,
	)
	request := httptest.NewRequest(
		http.MethodPost,
		"/v1/server/evaluate",
		strings.NewReader(`{
			"serverId":"bnb",
			"sentAt":"2026-07-27T10:30:00Z",
			"players":[
				{"userId":"steam_00000000000000001","name":"Đặng 7749"}
			]
		}`),
	)
	request.Header.Set("Authorization", "Bearer server-token")
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("unicode player name rejected with HTTP %d", response.Code)
	}
}

func TestLauncherManifestEndpointPublishesRequiredVersions(t *testing.T) {
	store := NewStore(Config{
		AllowedMods: map[string]AllowedMod{},
		LauncherManifest: LauncherManifest{
			LauncherVersion:         "0.5.0",
			MinimumLauncherVersion:  "0.5.0",
			LauncherDownloadURL:     "https://downloads.minerua.net/Pal3Mien-setup.exe",
			LauncherSHA256:          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			PalVerifyVersion:        "0.5.0",
			RequiredPalworldBuildID: "24181527",
			PalworldVersion:         "v1.0.1.100619",
			ServerOnline:            true,
			ServerAddress:           "lantica.dathost.net:28709",
			WebsiteURL:              "https://minerua.net/",
			NewsURL:                 "https://minerua.net/tin-tuc",
		},
	})
	handler := NewHandler(
		store,
		"server-token",
		time.Now,
		nil,
	)
	request := httptest.NewRequest(
		http.MethodGet,
		"/v1/launcher/manifest",
		nil,
	)
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("manifest status: %d", response.Code)
	}
	if response.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf(
			"manifest must not be cached: %q",
			response.Header().Get("Cache-Control"),
		)
	}
	var manifest LauncherManifest
	if err := json.NewDecoder(response.Body).Decode(&manifest); err != nil {
		t.Fatalf("decode manifest: %v", err)
	}
	if manifest.RequiredPalworldBuildID != "24181527" ||
		manifest.PalVerifyVersion != "0.5.0" ||
		manifest.ServerAddress != "lantica.dathost.net:28709" ||
		!manifest.ServerOnline {
		t.Fatalf("unexpected manifest: %#v", manifest)
	}
}
